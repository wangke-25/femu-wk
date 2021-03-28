#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/pci/msix.h"
#include "hw/pci/msi.h"
#include "../nvme.h"
#include "ftl.h"

static void *ftl_thread(void *arg);


static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_gtd_ent(struct ssd *ssd, uint64_t tvpn)
{
    return ssd->gtd[tvpn];
}

static inline void set_gtd_ent(struct ssd *ssd, uint64_t tvpn, struct ppa *ppa)
{
    ssd->gtd[tvpn] = *ppa;
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
        ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

    assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    //QTAILQ_INIT(&lm->victim_line_list);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;

        line->type = -1;              //init line free
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd, int type)
{
    //struct write_pointer *wpp = &ssd->wp;
    struct write_pointer *wpp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;
    if(type == DATA_PAGE)
        wpp = &ssd->wp;
    else if(type == TRANS_PAGE)
        wpp = &ssd->trans_wp;
    
    /* make sure lines are already initialized by now */
    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    curline->type = type;

    /* wpp->curline is always our onging line for writes */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd, int type)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        printf("FEMU-FTL: Error, there is no free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    curline->type = type;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd, int type)
{
    struct ssdparams *spp = &ssd->sp;
    //struct write_pointer *wpp = &ssd->wp;
    struct write_pointer *wpp;
    struct line_mgmt *lm = &ssd->lm;
    if(type == DATA_PAGE)
    {
        wpp = &ssd->wp;
        ssd->cb_info->nand_w_cnt++;
    }
    else if(type == TRANS_PAGE)
    {
        wpp = &ssd->trans_wp;
        ssd->cb_info->nand_wt_cnt++;
    }

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    //printf("Coperd,curline,vpc:%d,ipc:%d\n", wpp->curline->vpc, wpp->curline->ipc);
                    assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    //QTAILQ_INSERT_TAIL(&lm->victim_line_list, wpp->curline, entry);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                /* TODO: how should we choose the next block for writes */
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd, type);
                if (!wpp->curline) {
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                assert(wpp->pg == 0);
                assert(wpp->lun == 0);
                assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                assert(wpp->pl == 0);
            }
        }
    }
    //printf("Next,ch:%d,lun:%d,blk:%d,pg:%d\n", wpp->ch, wpp->lun, wpp->blk, wpp->pg);
}

static struct ppa get_new_page(struct ssd *ssd, int type)
{
    //struct write_pointer *wpp = &ssd->wp;
    struct write_pointer *wpp;
    struct ppa ppa;
    if(type == DATA_PAGE)
        wpp = &ssd->wp;
    else if(type == TRANS_PAGE)
        wpp = &ssd->trans_wp;

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //assert(is_power_of_2(spp->luns_per_ch));
    //assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 256;
    //spp->blks_per_pl = 256; /* 16GB */
    spp->blks_per_pl = 640; /* 40GB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    int i;

    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    int i;

    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    int i;

    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    int i;

    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    int i;

    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

#ifdef DFTL
static void ssd_init_gtd(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;

    /*ssd->gtd = g_malloc0(sizeof(struct ppa) * (spp->tt_pgs / ENTRY_PER_PAGE) + 1);
    for (i = 0; i < (spp->tt_pgs / ENTRY_PER_PAGE); i++) {
        ssd->gtd[i].ppa = UNMAPPED_PPA;
    }*/
    ssd->gtd = g_malloc0(sizeof(struct ppa) * (spp->tt_pgs / (ENTRY_PER_PAGE/2)) + 1);
    for (i = 0; i < (spp->tt_pgs / (ENTRY_PER_PAGE/2) + 1); i++) {
        ssd->gtd[i].ppa = UNMAPPED_PPA;
    }
}
#endif

static void ssd_init_maptbl(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;
    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n)
{
    int i;
    struct ssd *ssd = &n->ssd;
    struct ssdparams *spp = &ssd->sp;

    assert(ssd);

    ssd_init_params(spp);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

#ifdef DFTL
    /* init gtd */
    ssd_init_gtd(ssd);

    /* init dftl */
    init_dftl(ssd);
#endif

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd, DATA_PAGE);

#ifdef DFTL
    /* init trans write pointer */
    ssd_init_write_pointer(ssd, TRANS_PAGE);
#endif

    /**initialize chunkbuffer**/
    init_chunkbuffer(ssd);

    qemu_thread_create(&ssd->ftl_thread, "ftl_thread", ftl_thread, n, QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch &&
            pl >= 0 && pl < spp->pls_per_lun && blk >= 0 &&
            blk < spp->blks_per_pl && pg >= 0 && pg < spp->pgs_per_blk &&
            sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa,
        struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    //struct ssd_channel *ch = get_ch(ssd, ppa);
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */

        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        printf("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

static uint64_t ssd_advance_status_delay(struct ssd *ssd, struct ppa *ppa,
        struct nand_cmd *ncmd, uint64_t lat)
{
    ncmd->stime += lat;
    return ssd_advance_status(ssd, ppa, ncmd) + lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    line->vpc--;
    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        //QTAILQ_INSERT_TAIL(&lm->victim_line_list, line, entry);
        lm->victim_line_cnt++;
    }
}

/* update SSD status about one page from PG_FREE -> PG_VALID */
static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    if(pg->status != PG_FREE)
    {
        printf("ch: %d, lun: %d, pl: %d, blk: %d, pg: %d\n", ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);
        printf("wp: ch: %d, lun: %d, pl: %d, blk: %d, pg: %d\n", ssd->wp.ch, ssd->wp.lun, ssd->wp.pl, ssd->wp.blk, ssd->wp.pg);
        printf("twp: ch: %d, lun: %d, pl: %d, blk: %d, pg: %d\n", ssd->trans_wp.ch, ssd->trans_wp.lun, ssd->trans_wp.pl, ssd->trans_wp.blk, ssd->trans_wp.pg);
        printf("pg_status: %d\n", pg->status);
    }
    assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    assert(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    assert(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
    line->vpc++;
}

/* only for erase, reset one block to free state */
static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;
    int i;

    for (i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

/* assume the read data will staged in DRAM and then flushed back to NAND */
static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

static uint64_t gc_write_trans_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t t_vpn = get_rmap_ent(ssd, old_ppa);

    new_ppa = get_new_page(ssd, TRANS_PAGE);
    /* update gtd */
    set_gtd_ent(ssd, t_vpn, &new_ppa);
    /* update rgtd */
    set_rmap_ent(ssd, t_vpn, &new_ppa);
    
    mark_page_valid(ssd, &new_ppa);

    ssd_advance_write_pointer(ssd, TRANS_PAGE);
    
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    //struct ssd_channel *new_ch;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    /* first read out current mapping info */
    //set_rmap(ssd, lpn, new_ppa);

    assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd, DATA_PAGE);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    //mark_page_invalid(ssd, old_ppa);
    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd, DATA_PAGE);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
    //new_ch = get_ch(ssd, &new_ppa);
    //new_ch->gc_endtime = new_ch->next_ch_avail_time;

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

/* TODO: now O(n) list traversing, optimize it later */
static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;
    //int max_ipc = 0;
    //int cnt = 0;

#if 0
    if (QTAILQ_EMPTY(&lm->victim_line_list)) {
        return NULL;
    }

    QTAILQ_FOREACH(line, &lm->victim_line_list, entry) {
        //printf("Coperd,%s,victim_line_list[%d],ipc=%d,vpc=%d\n", __func__, ++cnt, line->ipc, line->vpc);
        if (line->ipc > max_ipc) {
            victim_line = line;
            max_ipc = line->ipc;
        }
    }
#endif

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        //printf("Coperd,select a victim line: ipc=%d (< 1/8)\n", victim_line->ipc);
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    //QTAILQ_REMOVE(&lm->victim_line_list, victim_line, entry);
    lm->victim_line_cnt--;
    //printf("Coperd,%s,victim_line_list,chooose-victim-block,id=%d,ipc=%d,vpc=%d\n", __func__, victim_line->id, victim_line->ipc, victim_line->vpc);

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa, int type)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    int pg;

    for (pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            if(type == DATA_PAGE)
                gc_write_page(ssd, ppa);
            else if(type == TRANS_PAGE)
                gc_write_trans_page(ssd, ppa);
            cnt++;
        }
    }

    assert(blk->vpc == cnt);
    /* do we do "erase" here? */
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    line->type = -1;                //mark line free
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
    //printf("Coperd,%s,one more free line,free_line_cnt=%d\n", __func__, lm->free_line_cnt);
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    //struct ssd_channel *chp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
    int type;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        ///////printf("FEMU-FTL: failed to get a victim line!\n");
        //abort();
        return -1;
    }

    type = victim_line->type;                       //get line's type
    ppa.g.blk = victim_line->id;
    /*printf("Coperd,%s,FTL,GCing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n",
            ssd->ssdname, ppa.g.blk, victim_line->ipc, ssd->lm.victim_line_cnt,
            ssd->lm.full_line_cnt, ssd->lm.free_line_cnt);*/
    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            //chp = get_ch(ssd, &ppa);
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa, type);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            //chp->gc_endtime = chp->next_ch_avail_time;
            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = &n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    while (1) {
        for (int i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }
            assert(req);
            switch (req->is_write) {
                case 1:
                    lat = ssd_write(ssd, req);
                    break;
                case 0:
                    lat = ssd_read(ssd, req);
#if 0
                if (lat >= 20000000) {
                    lat /= 4;
                }
#endif
                    break;
                default:
                    printf("FEMU: FTL received unkown request type, ERROR\n");
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }
}

#ifdef DFTL

int init_taichi(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    struct chunk_map_info *ckm_info = (struct chunk_map_info *)malloc(sizeof(struct chunk_map_info));
    ckm_info->bitmap = (char *)malloc(sizeof(char) * spp->tt_pgs);
    ckm_info->chunk_page_cnt = (int *)malloc(sizeof(int) * spp->tt_pgs/PAGES_PER_CHUNK + 1);

    int i;
    for(i = 0; i < spp->tt_pgs; i++)
        ckm_info->bitmap[i] = -1;
    memset(ckm_info->chunk_page_cnt, 0, sizeof(int) * spp->tt_pgs/PAGES_PER_CHUNK + 1);

    ckm_info->chunk_map_cnt = 0;
    ckm_info->page_map_cnt = 0;
    ckm_info->update_cnt = 0;

    ssd->mc_info->ckm_info = ckm_info;

    return 0;
}

int init_dftl(struct ssd *ssd)
{
    struct mapping_cache_info *mc_info = (struct mapping_cache_info *)malloc(sizeof(struct mapping_cache_info));
    assert(mc_info);
    
    mc_info->cur_size = 0;
    mc_info->max_size = MAX_MAPPING_CAHCHE_SIZE;

    mc_info->head = NULL;
    mc_info->tail = NULL;

    mc_info->dftl_rhit = 0;
    mc_info->dftl_rhit1 = 0;
    mc_info->dftl_rmiss = 0;
    mc_info->dftl_whit = 0;
    mc_info->dftl_whit1 = 0;
    mc_info->dftl_wmiss = 0;

    mc_info->r_delay = 0;

    mc_info->hashmap = (struct map_entry_node **)malloc(sizeof(struct map_entry_node *) * DFTL_HASH_MAP_SIZE);
    assert(mc_info->hashmap);

    for(int i = 0; i < DFTL_HASH_MAP_SIZE; i++)
        mc_info->hashmap[i] = NULL;

    ssd->mc_info = mc_info;

    init_taichi(ssd);

    return 0;
}

struct map_entry_node *dftl_lookup_hashmap(struct mapping_cache_info *mc_info, uint64_t tvpn)
{
    uint64_t hashkey = tvpn;
    uint64_t index = hashkey % DFTL_HASH_MAP_SIZE;

    struct map_entry_node *tmp;
    tmp = mc_info->hashmap[index];

    while(tmp != NULL)
    {
        if(tmp->tvpn == tvpn)
        {
            return tmp;
        }

        tmp = tmp->hashnext;
    }

    return tmp;
}

void dftl_add_2_hashmap(struct mapping_cache_info *mc_info, struct map_entry_node *new_node)
{
    //printf("2.0!\n");
    uint64_t hashkey = new_node->tvpn;
    uint64_t index = hashkey % DFTL_HASH_MAP_SIZE;

    struct map_entry_node *tmp;
    tmp = mc_info->hashmap[index];

    if(tmp == NULL) {
        mc_info->hashmap[index] = new_node;
    }
    else {
        while(tmp->hashnext != NULL) {
            tmp = tmp->hashnext;
        }

        tmp->hashnext = new_node;
        new_node->hashpre = tmp;
    }
}

void dftl_del_from_hashmap(struct mapping_cache_info *mc_info, struct map_entry_node *victim)
{
    uint64_t hashkey = victim->tvpn;
    uint64_t index = hashkey % DFTL_HASH_MAP_SIZE;
    
    if(mc_info->hashmap[index] == victim) {
        if(victim->hashpre == NULL) {
            mc_info->hashmap[index] = victim->hashnext;

            if(victim->hashnext != NULL) {
                victim->hashnext->hashpre = NULL;
            }
        }
        else
            printf("del_from_hashmap error!\n");
    }
    else {
        victim->hashpre->hashnext = victim->hashnext;
        if(victim->hashnext != NULL){
            victim->hashnext->hashpre = victim->hashpre;
        }
    }
    
    victim->hashnext = NULL;
    victim->hashpre = NULL;
}

int dftl_look_up(struct ssd *ssd, uint64_t lpn)
{
    struct mapping_cache_info* mc_info = ssd->mc_info;
    struct map_entry_node *tmp;
    uint64_t tvpn = lpn / ENTRY_PER_PAGE;
    uint64_t offset = lpn % ENTRY_PER_PAGE;

    tmp = dftl_lookup_hashmap(mc_info, tvpn);
    if(tmp != NULL) {
        if(tmp == mc_info->head);
        else if(tmp == mc_info->tail) {
            tmp->pre->next = NULL;
            mc_info->tail = tmp->pre;
            tmp->pre = NULL;
            tmp->next = mc_info->head;
            mc_info->head->pre = tmp;
            mc_info->head = tmp;
        }
        else {
            tmp->pre->next = tmp->next;
            tmp->next->pre = tmp->pre;
            tmp->pre = NULL;
            tmp->next = mc_info->head;
            mc_info->head->pre = tmp;
            mc_info->head = tmp;
        }

        if(tmp->bitmap[offset] == 1)
            return 1;
        else
            return 0;
    }

    return -1;
}

int dftl_lru_evict(struct mapping_cache_info *mc_info, struct map_entry_node *victim)
{
    if(victim == mc_info->head)
    {
        mc_info->head = NULL;
        mc_info->tail = NULL;
    }
    else
    {
        mc_info->tail = victim->pre;
        mc_info->tail->next = NULL;
    }

    dftl_del_from_hashmap(mc_info, victim);
#if DFTL_DEBUG
    printf("evict: %lu\n", victim->tvpn);
#endif
    return 0;
}

int dftl_lru_add_new_node(struct mapping_cache_info *mc_info, uint64_t lpn, int res)
{
    uint64_t tvpn = lpn / ENTRY_PER_PAGE;
    uint64_t offset = lpn % ENTRY_PER_PAGE;

    if(res == -1)
    {
        struct map_entry_node *new_node = (struct map_entry_node *)malloc(sizeof(struct map_entry_node));

        assert(new_node);

        memset(new_node->bitmap, 0, sizeof(char) * ENTRY_PER_PAGE);
        memset(new_node->dirty, 0, sizeof(char) * ENTRY_PER_PAGE);
        new_node->tvpn = 0;
        new_node->pre = NULL;
        new_node->next = NULL;
        new_node->hashpre = NULL;
        new_node->hashnext = NULL;
        new_node->entry_cnt[0] = 0;
        new_node->entry_cnt[1] = 0;
        new_node->dirty_cnt[0] = 0;
        new_node->dirty_cnt[1] = 0;

        //new_node->entry_cnt[0] = ENTRY_PER_PAGE;
        //for(int i = 0; i < ENTRY_PER_PAGE; i++)
            //new_node->bitmap[i] = 1;
        if(offset < ENTRY_PER_PAGE/2)
            new_node->entry_cnt[0]++;
        else
            new_node->entry_cnt[1]++;
        new_node->tvpn = tvpn;
        new_node->bitmap[offset] = 1;

        if(mc_info->head == NULL)
        {
            mc_info->head = new_node;
            mc_info->tail = new_node;
        }
        else
        {
            mc_info->head->pre = new_node;
            new_node->next = mc_info->head;
            mc_info->head = new_node;
        }

        dftl_add_2_hashmap(mc_info, new_node);

        mc_info->cur_size++;
#if DFTL_DEBUG
        printf("add： %lu\n", new_node->tvpn);
#endif
    }
    else if(res == 0)
    {
        struct map_entry_node *tmp = mc_info->head;
        assert(tvpn == tmp->tvpn);
        tmp->bitmap[offset] = 1;
        if(offset < ENTRY_PER_PAGE/2)
            tmp->entry_cnt[0]++;
        else
            tmp->entry_cnt[1]++;

        mc_info->cur_size++;
    }
    else
        printf("error in dftl_lru_add_new_node()!\n");

    return 0;
}

void dftl_set_dirty(struct mapping_cache_info *mc_info, uint64_t lpn)
{
    struct map_entry_node *tmp = mc_info->head;
    uint64_t tvpn = lpn / ENTRY_PER_PAGE;
    uint64_t offset = lpn % ENTRY_PER_PAGE;
    if(tmp && tvpn == tmp->tvpn)
    {
        if(tmp->dirty[offset] == 0){
            tmp->dirty[offset] = 1;
            if(offset < ENTRY_PER_PAGE/2)
                tmp->dirty_cnt[0]++;
            else
                tmp->dirty_cnt[1]++;
        }
    }
    else
        printf("error in dftl_set_dirty()\n");
}

uint64_t dftl_load_entry(struct ssd *ssd, struct ppa ppa, int64_t stime, uint64_t lat)
{
        struct nand_cmd srd;
        srd.type = TRANS_IO;
        srd.cmd = NAND_READ;
        srd.stime = stime;
        return ssd_advance_status_delay(ssd, &ppa, &srd, lat);
}

uint64_t write_back_trans_page(struct ssd *ssd, uint64_t tvpn, int64_t stime, int dirty_cnt)
{
    struct ppa ppa;
    uint64_t sublat = 0;

    ppa = get_gtd_ent(ssd, tvpn);
    if(mapped_ppa(&ppa))
    {
        if(dirty_cnt < ENTRY_PER_PAGE/2)
            dftl_load_entry(ssd, ppa, stime, 0);
        mark_page_invalid(ssd, &ppa);
        set_rmap_ent(ssd, INVALID_LPN, &ppa);
    }
    /* new write */
    /* find a new page */
    ppa = get_new_page(ssd, TRANS_PAGE);
    /* update gtd */
    set_gtd_ent(ssd, tvpn, &ppa);
    /* update rmap */
    set_rmap_ent(ssd, tvpn, &ppa);

    mark_page_valid(ssd, &ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd, TRANS_PAGE);

    struct nand_cmd swr;
    swr.type = TRANS_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = stime;
    /* get latency statistics */
    sublat = ssd_advance_status(ssd, &ppa, &swr);
#if DFTL_DEBUG
    printf("writeback trans page: %lu\n", tvpn);
#endif
    return sublat;
}

int dftl_test(struct ssd *ssd, uint64_t lpn, int op)
{
    if(op == WRITE)
        printf("WRITE lpn：%lu\n", lpn);
    else if(op == READ)
        printf("READ  lpn: %lu\n", lpn);
    
    printf("mc cursize: %d,", ssd->mc_info->cur_size/ENTRY_PER_PAGE);
    if(ssd->mc_info->head)
        printf("%lu, ", ssd->mc_info->head->tvpn);
    if(ssd->mc_info->tail)
        printf("%lu\n", ssd->mc_info->tail->tvpn);

    return 0;
}

uint64_t dftl_translate(struct ssd* ssd, uint64_t lpn, int op, int64_t stime)
{
    struct mapping_cache_info* mc_info = ssd->mc_info;
    struct map_entry_node* victim = NULL;
    int add_size = ENTRY_PER_PAGE;
    uint64_t tvpn = lpn / ENTRY_PER_PAGE;
    uint64_t offset = lpn % ENTRY_PER_PAGE;
    uint64_t sublat = 0;
    uint64_t maxlat = 0;
    struct ppa ppa;

#if DFTL_DEBUG
    dftl_test(ssd, lpn, op);
#endif
    int res;
    res = dftl_look_up(ssd, lpn);
    if(res <= 0)
    {
        while(mc_info->cur_size + add_size > mc_info->max_size)
        {
            victim = mc_info->tail;

            if(victim)
            {
                if(victim->dirty_cnt[0] > 0)
                    sublat = write_back_trans_page(ssd, victim->tvpn, stime, 0);
                dftl_lru_evict(ssd->mc_info, victim);
                mc_info->cur_size -= victim->entry_cnt[0];

                free(victim);
                victim = NULL;
            }

            maxlat = maxlat > sublat ? maxlat : sublat;
        }
        ppa = get_gtd_ent(ssd, tvpn);
        if(mapped_ppa(&ppa))             //tvpn-ppn exist
            maxlat = dftl_load_entry(ssd, ppa, stime, maxlat);

        dftl_lru_add_new_node(ssd->mc_info, lpn, res);
    }
    if(op == WRITE)
        dftl_set_dirty(ssd->mc_info, lpn);                     //op => setdirty

#if DFTL_DEBUG
    dftl_test(ssd, lpn, op);
#endif

    return maxlat;
}

uint64_t taichi_add2_pagemap(struct ssd *ssd, uint64_t lpn, int64_t stime, int op, int res)
{
    struct mapping_cache_info *mc_info = ssd->mc_info;
    struct map_entry_node *victim;
    struct ppa ppa;
    uint64_t sublat0 = 0;
    uint64_t sublat1 = 0;
    uint64_t maxlat = 0;
    int add_size = 1;

    while(mc_info->cur_size + add_size > mc_info->max_size)
    {
        victim = mc_info->tail;

        if(victim)
        {
            sublat0 = 0;
            sublat1 = 0;
            if(victim->dirty[0] > 0)
                sublat0 = write_back_trans_page(ssd, (victim->tvpn * 2), stime, victim->dirty[0]);
            if(victim->dirty[1] > 0)
                sublat1 = write_back_trans_page(ssd, (victim->tvpn * 2 + 1), stime, victim->dirty[1]);
            dftl_lru_evict(ssd->mc_info, victim);
            mc_info->cur_size -= victim->entry_cnt[0];
            mc_info->cur_size -= victim->entry_cnt[1];

            free(victim);
            victim = NULL;
            sublat1 = sublat0 > sublat1? sublat0 : sublat1;
        }
        maxlat = maxlat > sublat1? maxlat : sublat1;
    }
    
    ppa = get_gtd_ent(ssd, GTD_INDEX(lpn));
    if(mapped_ppa(&ppa))
        maxlat = dftl_load_entry(ssd, ppa, stime, maxlat);

    dftl_lru_add_new_node(ssd->mc_info, lpn, res);
    
    if(op == WRITE)
        dftl_set_dirty(ssd->mc_info, lpn); 

    return maxlat;
}

void taichi_del_from_pagemap(struct ssd *ssd, uint64_t lpn)
{
    struct mapping_cache_info *mc_info = ssd->mc_info;
    uint64_t tvpn = lpn / ENTRY_PER_PAGE;
    uint64_t offset = lpn % ENTRY_PER_PAGE;
    int res;

    res = dftl_look_up(ssd, lpn);
    if(res == 1)
    {
        mc_info->dftl_whit1++;
        struct map_entry_node *tmp = mc_info->head;
        if(tmp)
        {
            if(tmp->bitmap[offset] == 1)
            {
                tmp->bitmap[offset] = 0;
                if(offset < ENTRY_PER_PAGE/2)
                    tmp->entry_cnt[0]--;
                else
                    tmp->entry_cnt[1]--;

                mc_info->cur_size--;
                
                if(tmp->dirty[offset] == 1)
                {
                    tmp->dirty[offset] = 0;
                    if(offset < ENTRY_PER_PAGE/2)
                        tmp->dirty_cnt[0]--;
                    else
                        tmp->dirty_cnt[1]--;
                }
                
                if(tmp->entry_cnt[0] + tmp->entry_cnt[1] == 0)
                {
                    if(tmp->dirty_cnt[0] + tmp->dirty_cnt[1] == 0)
                    {
                        if (tmp == mc_info->head || tmp == mc_info->tail)
                        {
                            if (tmp == mc_info->head && tmp == mc_info->tail)
                            {
                                mc_info->head = NULL;
                                mc_info->tail = NULL;
                            }
                            else if (tmp == mc_info->head)
                            {
                                tmp->next->pre = NULL;
                                mc_info->head = tmp->next;
                            }
                            else if (tmp == mc_info->tail)
                            {
                                tmp->pre->next = NULL;
                                mc_info->tail = tmp->pre;
                            }
                        }
                        else
                        {
                            tmp->pre->next = tmp->next;
                            tmp->next->pre = tmp->pre;
                        }
                        dftl_del_from_hashmap(ssd->mc_info, tmp);

                        free(tmp);
                        tmp = NULL;
                    }
                    else
                        printf("error in taichi_del_from_pagemap1!\n");
                }
            }
            else
                printf("error in taichi_del_from_pagemap2!\n");
        }
    }
    else
    {
        mc_info->dftl_wmiss++;
    }
    
    //else
        //printf("not find page: %lu\n", lpn);
}

uint64_t taichi_trans_read(struct ssd *ssd, uint64_t lpn, int64_t stime)
{
    struct mapping_cache_info *mc_info = ssd->mc_info;
    uint64_t lat = 0;
    struct ppa ppa;

    //printf("taichi_trans_read: lpn: %lu\n", lpn);
    if(mc_info->ckm_info->bitmap[lpn] == 1)
    {
        lat = 0;
        //printf("taichi_trans_read-1\n");
        mc_info->dftl_rhit++;
    }
    else {
        int res = dftl_look_up(ssd, lpn);
        if(res != 1) {
            lat = taichi_add2_pagemap(ssd, lpn, stime, READ, res);
            //ppa = get_gtd_ent(ssd, GTD_INDEX(lpn));
            //if(mapped_ppa(&ppa))
                //lat = dftl_load_entry(ssd, ppa, stime, 0);
            
            //dftl_lru_add_new_node(ssd->mc_info, lpn, res);
            //printf("lat = %lu\n", lat);
            mc_info->dftl_rmiss++;
        }
        else
        {
            lat = 0;
            mc_info->dftl_rhit1++;
        }
            
        //printf("taichi_trans_read-2\n");
    }

    return lat;
}

static inline void taichi_test(struct ssd *ssd, int flag)
{
    struct mapping_cache_info *mc_info = ssd->mc_info;
    struct chunk_map_info *ckm_info = mc_info->ckm_info;
    struct map_entry_node *tmp = NULL;

#if 0
    if(mc_info->head)
    {
        if(mc_info->head == mc_info->head->next)
            printf("flag = %d, head: %lu\n", flag, mc_info->head->tvpn);
    }

    if(mc_info->cur_size > ckm_info->page_map_cnt)
    {
        if(flag != 0)
            printf("flag = %d\n", flag);
        printf("errorrrrrrr!!!!  cursize: %d, page_map_cnt: %d\n", mc_info->cur_size, ckm_info->page_map_cnt);
        if(mc_info->head && mc_info->tail)
            printf("head:%lu, tail:%lu\n", mc_info->head->tvpn, mc_info->tail->tvpn);
        int cnt = 0;
        tmp = mc_info->head;
        while(tmp != NULL)
        {
            printf("%lu->", tmp->tvpn);
            cnt += (tmp->entry_cnt[0] + tmp->entry_cnt[1]);
            tmp = tmp->next;
        }
        printf("\nerrorrrrrrr!!!!  cnt: %d\n", cnt);

        assert(mc_info->cur_size == cnt);
    }

    assert(mc_info->cur_size <= ckm_info->page_map_cnt);
#else
    if(flag == 0)
        printf("before del:\n");
    else
        printf("after del:\n");
    tmp = mc_info->head;
    if(tmp)
        printf("head: %lu, tail: %lu, size: %d\n%lu", mc_info->head->tvpn, mc_info->tail->tvpn,tmp->entry_cnt[0]+tmp->entry_cnt[1], tmp->tvpn);
    tmp = tmp->next;
    while(tmp)
    {
        printf("->%lu", tmp->tvpn);
        tmp = tmp->next;
    }
    printf("\n");
#endif
}

uint64_t taichi_trans_write(struct ssd *ssd, struct chunk_node *victim, int64_t stime)
{
    struct mapping_cache_info *mc_info = ssd->mc_info;
    struct chunk_map_info *ckm_info = mc_info->ckm_info;
    int update_size = 0;
    uint64_t start_lpn = victim->lcn * PAGES_PER_CHUNK;
    int cur_size = ckm_info->chunk_page_cnt[victim->lcn];
    int write_size = victim->size;
    uint64_t lat = 0;
    uint64_t maxlat = 0;

    int i;
    for(i = 0; i < PAGES_PER_CHUNK; i++)
    {
        if(victim->bitmap[i] == 1 && ckm_info->bitmap[start_lpn+i] == 1)
            update_size++;
    }

    if(write_size <= cur_size - update_size)                    //new page and update chunk page => page map
    {
        for(i = 0; i < PAGES_PER_CHUNK; i++)
        {
            lat = 0;
            if(victim->bitmap[i] == 1)
            {
                if(ckm_info->bitmap[start_lpn+i] == 1)           //chunk map->page map(update)
                {
                    ckm_info->chunk_page_cnt[victim->lcn]--;

                    ckm_info->update_cnt++;
                    ckm_info->chunk_map_cnt--;
                    ckm_info->page_map_cnt++;
                }
                else if(ckm_info->bitmap[start_lpn+i] == 0)    //page map->page map(update)
                {
                    ckm_info->update_cnt++;
                }
                else if(ckm_info->bitmap[start_lpn+i] == -1)    //null->page map
                {
                    ckm_info->page_map_cnt++;
                }
                else
                    printf("update map error1!\n");
                
                int res = dftl_look_up(ssd, start_lpn+i);
                
                if(ckm_info->bitmap[start_lpn+i] == 1)
                    mc_info->dftl_whit++;
                else
                {
                    if(res == 1)
                        mc_info->dftl_whit1++;
                    else
                        mc_info->dftl_wmiss++;
                }

                if(res == 1)
                {
                    dftl_set_dirty(ssd->mc_info, start_lpn+i);
                }
                else
                    lat = taichi_add2_pagemap(ssd, start_lpn+i, stime, WRITE, res);
                ckm_info->bitmap[start_lpn+i] = 0;
            }
            maxlat = maxlat > lat ? maxlat : lat;
        }
        //assert((cur_size - update_size) == ckm_info->chunk_page_cnt[victim->lcn]);
    }
    else                                                     //new page and update page => chunk map
    {
        int flag = 0;
        for(i = 0; i < PAGES_PER_CHUNK; i++)
        {
            lat = 0;
            if(ckm_info->bitmap[start_lpn+i] == 1)
            {
                if(victim->bitmap[i] == 0)                  //chunk map->page map
                {
                    int res = dftl_look_up(ssd, start_lpn+i);
                    if(res != 1)
                        lat = taichi_add2_pagemap(ssd, start_lpn+i, stime, WRITE, res);
                    else
                        printf("update map error2!");
                    
                    ckm_info->bitmap[start_lpn+i] = 0;
                    ckm_info->chunk_page_cnt[victim->lcn]--;

                    ckm_info->chunk_map_cnt--;
                    ckm_info->page_map_cnt++;
                    flag = 1;
                }
                else                                        //chunk map->chunk map(update)
                {
                    ckm_info->update_cnt++;
                    mc_info->dftl_whit++;
                    flag = 2;
                }
            }
            else if(ckm_info->bitmap[start_lpn+i] == 0)
            {
                if(victim->bitmap[i] == 1)                  //page map->chunk map(update)
                {
                    //printf("delete page:%lu from page map\n", start_lpn+i);
                    //taichi_test(ssd, 0);
                    taichi_del_from_pagemap(ssd, start_lpn+i);
                    //taichi_test(ssd, 1);
                    ckm_info->bitmap[start_lpn+i] = 1;
                    ckm_info->chunk_page_cnt[victim->lcn]++;

                    ckm_info->chunk_map_cnt++;
                    ckm_info->page_map_cnt--;
                    ckm_info->update_cnt++;
                    flag = 3;
                }
            }
            else if(ckm_info->bitmap[start_lpn+i] == -1)    //null->chunk map
            {
                if(victim->bitmap[i] == 1)
                {
                    ckm_info->bitmap[start_lpn+i] = 1;
                    ckm_info->chunk_page_cnt[victim->lcn]++;

                    ckm_info->chunk_map_cnt++;
                    mc_info->dftl_wmiss++;
                    flag = 4;
                }
            }
            maxlat = maxlat > lat ? maxlat : lat;
            //taichi_test(ssd, flag);
        }
        //assert(write_size == ckm_info->chunk_page_cnt[victim->lcn]);
    }

    //if((ckm_info->page_map_cnt + ckm_info->chunk_map_cnt + ckm_info->update_cnt) % 100000 == 0 )
        //printf("taichi chunk map rate: %.5f, chunk: %lu, page: %lu, update: %lu\n", (double)ckm_info->chunk_map_cnt/(ckm_info->page_map_cnt + ckm_info->chunk_map_cnt), 
            //ckm_info->chunk_map_cnt, ckm_info->page_map_cnt, ckm_info->update_cnt);

    //taichi_test(ssd, 0);
    return maxlat;
}

uint64_t taichi_translate(struct ssd *ssd, uint64_t lpn, struct chunk_node *victim, int op, int64_t stime)
{
    uint64_t lat = 0;
    if(op == READ)
        lat = taichi_trans_read(ssd, lpn, stime);
    else if(op == WRITE)
        lat = taichi_trans_write(ssd, victim, stime);
    else
        printf("error in taichi_translate!");

    return lat;
}

#endif

/**wk**/
int init_chunkbuffer(struct ssd *ssd)
{
	struct chunk_buffer_info *cb_info = (struct chunk_buffer_info *)malloc(sizeof(struct chunk_buffer_info));
	assert(cb_info);

	cb_info->cur_size = 0;
	cb_info->max_size = MAX_CHUNK_BUFFER_SIZE;
	cb_info->head = NULL;
	cb_info->tail = NULL;

    cb_info->rbuffer_hit = 0;
    cb_info->wbuffer_hit = 0;
    cb_info->rbuffer_miss = 0;
    cb_info->wbuffer_miss = 0;

    cb_info->wb_page = 0;

    cb_info->rdelay = 0;
    cb_info->wdelay = 0;
    cb_info->rcnt = 0;
    cb_info->wcnt = 0;

    cb_info->nand_w_cnt = 0;
    cb_info->nand_wt_cnt = 0;

    cb_info->hashmap = (struct chunk_node **)malloc(sizeof(struct chunk_node *) * HASH_MAP_SIZE);
    assert(cb_info->hashmap);

    for(int i = 0; i < HASH_MAP_SIZE; i++)
        cb_info->hashmap[i] = NULL;

	ssd->cb_info = cb_info;

	return 0;
}

/**wk**/
struct chunk_node *lookup_hashmap(struct chunk_buffer_info *cb_info, uint64_t lcn)
{
    //printf("1.0!\n");
    uint64_t hashkey = lcn;
    uint64_t index = hashkey % HASH_MAP_SIZE;

    struct chunk_node *tmp;
    tmp = cb_info->hashmap[index];

    while(tmp != NULL)
    {
        if(tmp->lcn == lcn)
        {
            //printf("1.1!\n");
            return tmp;
        }
        tmp = tmp->hashnext;
    }

    //printf("1.2!\n");
    return tmp;
}

/**wk**/
void add_2_hashmap(struct chunk_buffer_info *cb_info, struct chunk_node *new_node)
{
    //printf("2.0!\n");
    uint64_t hashkey = new_node->lcn;
    uint64_t index = hashkey % HASH_MAP_SIZE;

    struct chunk_node *tmp;
    tmp = cb_info->hashmap[index];

    if(tmp == NULL) {
        cb_info->hashmap[index] = new_node;
    }
    else {
        while(tmp->hashnext != NULL) {
            tmp = tmp->hashnext;
        }

        tmp->hashnext = new_node;
        new_node->hashpre = tmp;
    }
}

void test_hashmap(struct chunk_buffer_info *cb_info, uint64_t index, struct chunk_node *victim, int start_end)
{
    struct chunk_node *tmp = cb_info->hashmap[index];
    if(tmp != NULL && tmp->hashnext != NULL) {
        if(start_end == 0)
            printf("before del: ");
        else if(start_end == 1)
            printf("after del: ");
        while(tmp != NULL) {
            if(tmp->hashnext != NULL)
                printf("%d-> ", tmp->lcn);
            else
                printf("%d", tmp->lcn);
            tmp = tmp->hashnext;
        }
        printf("\n");
        if(start_end == 0)
            printf("del: %d\n", victim->lcn);
    }
}

/**wk**/
void del_from_hashmap(struct chunk_buffer_info *cb_info, struct chunk_node *victim)
{
    //printf("3.0!\n");
    uint64_t hashkey = victim->lcn;
    uint64_t index = hashkey % HASH_MAP_SIZE;

    //test_hashmap(cb_info, index, victim, 0);
    
    if(cb_info->hashmap[index] == victim) {
        if(victim->hashpre == NULL) {
            cb_info->hashmap[index] = victim->hashnext;

            if(victim->hashnext != NULL) {
                victim->hashnext->hashpre = NULL;
            }
        }
        else
            printf("del_from_hashmap error!\n");
    }
    else {
        //test_hashmap(cb_info, index, victim, 0);
        victim->hashpre->hashnext = victim->hashnext;
        if(victim->hashnext != NULL){
            victim->hashnext->hashpre = victim->hashpre;
        }
        //test_hashmap(cb_info, index, victim, 1);
    }
    
    victim->hashnext = NULL;
    victim->hashpre = NULL;
    //test_hashmap(cb_info, index, victim, 1);
    //printf("3.1!\n");
}

/**wk**/
int lru_lookup(struct ssd *ssd, uint64_t lpn)
{
    struct chunk_buffer_info *cb_info = ssd->cb_info;
    struct chunk_node *tmp;
    uint64_t lcn = lpn / PAGES_PER_CHUNK;
    uint64_t offset = lpn % PAGES_PER_CHUNK;

    tmp = lookup_hashmap(cb_info, lcn);
#if 0
    struct chunk_node *cur = cb_info->head;
    while(cur != NULL)
    {
        if(cur->lcn == lcn)
            break;
        cur = cur->next;
    }
    /*if(cur != tmp) {
        if(tmp != NULL && cur != NULL)
            printf("lcn: %d, hash：%d, cur: %d\n", lcn, tmp->lcn, cur->lcn);
        printf("error in hashmap!\n");
    }*/
    tmp = cur;
#endif
    if(tmp != NULL) {
        if(tmp == cb_info->head);
        else if(tmp == cb_info->tail) {
            tmp->pre->next = NULL;
            cb_info->tail = tmp->pre;
            tmp->pre = NULL;
            tmp->next = cb_info->head;
            cb_info->head->pre = tmp;
            cb_info->head = tmp;
        }
        else {
            tmp->pre->next = tmp->next;
            tmp->next->pre = tmp->pre;
            tmp->pre = NULL;
            tmp->next = cb_info->head;
            cb_info->head->pre = tmp;
            cb_info->head = tmp;
        }

        if(tmp->bitmap[offset] == 1)
            return 1;
        else
            return 0;
    }

    return -1;
}

/**wk**/
int lru_evict(struct chunk_buffer_info *cb_info, struct chunk_node *victim)
{
    if(victim == cb_info->head)
    {
        cb_info->head = NULL;
        cb_info->tail = NULL;
    }
    else
    {
        cb_info->tail = victim->pre;
        cb_info->tail->next = NULL;
    }

    del_from_hashmap(cb_info, victim);
    
    //printf("lru_evict()!\n");
    return 0;
}

/**wk**/
int lru_add_new_node(struct chunk_buffer_info *cb_info, uint64_t lpn, int res)
{
    uint64_t lcn = lpn / PAGES_PER_CHUNK;
    uint64_t offset = lpn % PAGES_PER_CHUNK;

    if(cb_info->head == NULL)
        res = -1;

    if(res == 0)
    {
        struct chunk_node *tmp = cb_info->head;
        tmp->bitmap[offset] = 1;
        tmp->size++;
    }
    else if(res == -1)
    {
        struct chunk_node *new_node = (struct chunk_node *)malloc(sizeof(struct chunk_node));

        assert(new_node);

        memset(new_node->bitmap, 0, sizeof(char) * PAGES_PER_CHUNK);
        new_node->size = 0;
        new_node->lcn = 0;
        new_node->pre = NULL;
        new_node->next = NULL;
        new_node->hashpre = NULL;
        new_node->hashnext = NULL;

        new_node->bitmap[offset] = 1;
        new_node->lcn = lcn;
        new_node->size++;

        if(cb_info->head == NULL)
        {
            cb_info->head = new_node;
            cb_info->tail = new_node;
        }
        else
        {
            cb_info->head->pre = new_node;
            new_node->next = cb_info->head;
            cb_info->head = new_node;
        }

        add_2_hashmap(cb_info, new_node);
    }
    else
        printf("error in lru_add_new_node()!\n");

    cb_info->cur_size++;
    return 0;
}

/**wk**/
uint64_t write_back_page(struct ssd *ssd, uint64_t lpn, int64_t stime)
{
    struct ppa ppa;
    uint64_t sublat = 0;

    ppa = get_maptbl_ent(ssd, lpn);
    if (mapped_ppa(&ppa)) {
        /* overwrite */
        /* update old page information first */
        //printf("Coperd,before-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
        mark_page_invalid(ssd, &ppa);
        //printf("Coperd,after-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
        set_rmap_ent(ssd, INVALID_LPN, &ppa);
    }

    /* new write */
    /* find a new page */
    ppa = get_new_page(ssd, DATA_PAGE);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &ppa);

    mark_page_valid(ssd, &ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd, DATA_PAGE);

    struct nand_cmd swr;
    swr.type = USER_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = stime;
    /* get latency statistics */
    sublat = ssd_advance_status(ssd, &ppa, &swr);

    //printf("write_back_page()!\n");
    return sublat;
}

/**wk**/
uint64_t write_back_chunk(struct ssd *ssd, struct chunk_node *victim, int64_t stime)
{
#if 1
    int i;
    uint64_t curlat = 0, maxlat = 0;
    uint64_t start_lpn = victim->lcn * PAGES_PER_CHUNK;

    for(i = 0; i < PAGES_PER_CHUNK; i++)
    {
        if(victim->bitmap[i] != 1)
            continue;

        uint64_t lpn = start_lpn + i;
        curlat = write_back_page(ssd, lpn, stime);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
//#ifdef DFTL
        //dftl_translate(ssd, lpn, WRITE, stime);
//#endif
    }

    ssd->cb_info->wb_page += victim->size;
#ifdef DFTL
    taichi_translate(ssd, 0, victim, WRITE, stime);
#endif

    return maxlat;
#else
    printf("lcn: %d, size: %d, bitmap: ", victim->lcn, victim->size);
    for(int i = 0; i < PAGES_PER_CHUNK; i++)
    {
        printf("%d, ", victim->bitmap[i]);
    }
    printf("\n");
    return 0;
#endif
}

/**wk**/
uint64_t ssd_buffer_read(struct ssd *ssd, uint64_t lpn, int64_t stime)
{
    uint64_t sublat = 0;
    struct ppa ppa;

    if(lru_lookup(ssd, lpn) == 1) {
        ssd->cb_info->rbuffer_hit++;
        sublat = BUFFER_HIT_LAT;
    }
    else {
        ssd->cb_info->rbuffer_miss++;
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //if(!mapped_ppa(&ppa))
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n", 
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            //continue;
            return 0;
        }
        uint64_t dftl_delay = 0;
#ifdef DFTL
        dftl_delay = taichi_translate(ssd, lpn, NULL, READ, stime);
        ssd->mc_info->r_delay += dftl_delay;
        
        // if((ssd->mc_info->dftl_rhit + ssd->mc_info->dftl_rmiss) % 1000000 == 0)
        // {
        //     double avg_delay = (double)(ssd->mc_info->r_delay)/(ssd->mc_info->dftl_rmiss+ssd->mc_info->dftl_rhit+ssd->mc_info->dftl_rhit1);
        //     double hitrate = (double)(ssd->mc_info->dftl_rhit + ssd->mc_info->dftl_rhit1)/(ssd->mc_info->dftl_rmiss+ssd->mc_info->dftl_rhit+ssd->mc_info->dftl_rhit1);

        //     printf("dftl rhit rate: %.5f, hit: %lu, miss: %lu, avg_delay: %.5f\n", hitrate, ssd->mc_info->dftl_rhit, ssd->mc_info->dftl_rmiss, avg_delay);
            
        //     ssd->mc_info->r_delay = 0;
        //     ssd->mc_info->dftl_rhit = 0;
        //     ssd->mc_info->dftl_rmiss = 0;
        // }

#endif
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = stime;
        sublat = ssd_advance_status_delay(ssd, &ppa, &srd, dftl_delay);

        /*ssd->cb_info->rdelay += sublat;
        ssd->cb_info->rcnt++;
        if(ssd->cb_info->rcnt % 1000000 == 0)
        {
            printf("avg read delay: %lu\n", ssd->cb_info->rdelay/ssd->cb_info->rcnt);
            ssd->cb_info->rdelay = 0;
            ssd->cb_info->rcnt = 0;
        }*/
        //if(dftl_delay != 0)
            //printf("read lat: %lu, lpn: %lu\n", sublat, lpn);
    }
    /*if((ssd->cb_info->rbuffer_hit + ssd->cb_info->rbuffer_miss) % 1000000 == 0)
    {
        double hitrate= (double)(ssd->cb_info->rbuffer_hit)/(ssd->cb_info->rbuffer_hit + ssd->cb_info->rbuffer_miss);
        printf("chunkbuffer rhit rate: %.5f, rbuffer hit:%lu, rbuffer miss: %lu\n", hitrate, ssd->cb_info->rbuffer_hit, ssd->cb_info->rbuffer_miss);
    }*/
    return sublat;
}

/**wk**/
uint64_t ssd_buffer_write(struct ssd *ssd, uint64_t lpn, int64_t stime)
{
    uint64_t sublat = 0;
    uint64_t maxlat = 0;
    int add_size = NODE_ADD_SIZE;
    struct chunk_buffer_info *cb_info = ssd->cb_info;
    struct chunk_node *victim = NULL;
    int res;

    res = lru_lookup(ssd, lpn);
    if(res == 1) {
        ssd->cb_info->wbuffer_hit++;
        maxlat = BUFFER_HIT_LAT;
    }
    else {
        ssd->cb_info->wbuffer_miss++;
        maxlat = BUFFER_HIT_LAT;
        while(cb_info->cur_size + add_size > cb_info->max_size) {
            victim = cb_info->tail;

            if(victim) {
                sublat = write_back_chunk(ssd, victim, stime);
                lru_evict(cb_info, victim);
                cb_info->cur_size -= victim->size;
                
                free(victim);
                victim = NULL;
            }
            maxlat = maxlat > sublat? maxlat : sublat;
        }

        lru_add_new_node(cb_info, lpn, res);

        //printf("buffer size: %d\n", cb_info->cur_size);
        //if(cb_info->head != NULL)
            //printf("head: %d, tail: %d\n", cb_info->head->lcn, cb_info->tail->lcn);

        if(cb_info->cur_size > cb_info->max_size)
            printf("error in handle_write_req()! cur_size: %d, max_size: %d\n", cb_info->cur_size, cb_info->max_size);
        
    }

    return maxlat;
}

/**wk**/
uint64_t ssd_buffer_management(struct ssd *ssd, uint64_t lpn, int64_t stime, int op)
{
    uint64_t lat = 0;
    if(op == WRITE)
    {
        lat = ssd_buffer_write(ssd, lpn, stime);
        
        /*ssd->cb_info->wdelay += lat;
        ssd->cb_info->wcnt++;
        if(ssd->cb_info->wcnt % 1000000 == 0)
        {
            printf("avg write delay: %lu\n", ssd->cb_info->wdelay/ssd->cb_info->wcnt);
            ssd->cb_info->wdelay = 0;
            ssd->cb_info->wcnt = 0;
            printf("wmiss: %lu, whit: %lu, wb: %lu, cb_size: %d\n", ssd->cb_info->wbuffer_miss, ssd->cb_info->wbuffer_hit, ssd->cb_info->wb_page, ssd->cb_info->cur_size);
#ifdef DFTL
            struct chunk_map_info *ckm_info = ssd->mc_info->ckm_info;
            double chunk_map_rate = (double)(ckm_info->chunk_map_cnt)/(ckm_info->page_map_cnt + ckm_info->chunk_map_cnt);
            printf("taichi chunk map rate: %.5f, chunk: %lu, page: %lu, update: %lu\n", chunk_map_rate, ckm_info->chunk_map_cnt, ckm_info->page_map_cnt, ckm_info->update_cnt);
            printf("mapping cache size: %d\n\n", ssd->mc_info->cur_size);
#endif
        }*/
    }
    else if(op == READ)
    {
        lat = ssd_buffer_read(ssd, lpn, stime);
        
        /*ssd->cb_info->rdelay += lat;
        ssd->cb_info->rcnt++;
        if(ssd->cb_info->rcnt % 1000000 == 0)
        {
            printf("avg read delay: %lu\n", ssd->cb_info->rdelay/ssd->cb_info->rcnt);
            ssd->cb_info->rdelay = 0;
            ssd->cb_info->rcnt = 0;
            //struct chunk_map_info *ckm_info = ssd->mc_info->ckm_info;
            //double chunk_map_rate = (double)(ckm_info->chunk_map_cnt)/(ckm_info->page_map_cnt + ckm_info->chunk_map_cnt);
            //printf("taichi chunk map rate: %.5f, chunk: %lu, page: %lu, update: %lu\n", chunk_map_rate, ckm_info->chunk_map_cnt, ckm_info->page_map_cnt, ckm_info->update_cnt);
        }*/
    }
    
    return lat;
}

void wk_print(struct ssd *ssd)
{
    printf("cb_whit: %lu, cb_wmiss: %lu, nand_w: %lu, nand_wt: %lu", ssd->cb_info->wbuffer_hit, ssd->cb_info->wbuffer_miss, ssd->cb_info->nand_w_cnt, ssd->cb_info->nand_wt_cnt);
    printf(", wa: %.5f\n", (double)(ssd->cb_info->nand_w_cnt+ssd->cb_info->nand_wt_cnt)/(ssd->cb_info->wbuffer_miss+ssd->cb_info->wbuffer_hit));
    // printf("wmiss: %lu, whit: %lu, wb: %lu, cb_size: %d\n", ssd->cb_info->wbuffer_miss, ssd->cb_info->wbuffer_hit, ssd->cb_info->wb_page, ssd->cb_info->cur_size);
#ifdef DFTL
    struct chunk_map_info *ckm_info = ssd->mc_info->ckm_info;
    double chunk_map_rate = (double)(ckm_info->chunk_map_cnt)/(ckm_info->page_map_cnt + ckm_info->chunk_map_cnt);
    struct mapping_cache_info *mc_info = ssd->mc_info;
    printf("taichi chunk map rate: %.5f, chunk: %lu, page: %lu, update: %lu\n", chunk_map_rate, ckm_info->chunk_map_cnt, ckm_info->page_map_cnt, ckm_info->update_cnt);
    uint64_t rhit_cnt = mc_info->dftl_rhit + mc_info->dftl_rhit1;
    uint64_t whit_cnt = mc_info->dftl_whit + mc_info->dftl_whit1;
    // double taichi_hit_rate = (double)(mc_info->dftl_whit+mc_info->dftl_whit1)/(mc_info->dftl_whit+mc_info->dftl_whit1+mc_info->dftl_wmiss);
    double taichi_hit_rate = (double)(rhit_cnt+whit_cnt)/(rhit_cnt+whit_cnt+mc_info->dftl_rmiss+mc_info->dftl_wmiss);
    printf("taichi hit rate: %.5f, chunk hit: %lu(%lu), page hit: %lu(%lu), miss: %lu(%lu)\n", taichi_hit_rate, mc_info->dftl_whit+mc_info->dftl_rhit, mc_info->dftl_whit,  
    mc_info->dftl_whit1+mc_info->dftl_rhit1, mc_info->dftl_whit1, mc_info->dftl_wmiss+mc_info->dftl_rmiss, mc_info->dftl_wmiss);

    mc_info->dftl_rhit = 0;
    mc_info->dftl_rhit1 = 0;
    mc_info->dftl_rmiss = 0;
    mc_info->dftl_whit = 0;
    mc_info->dftl_whit1 = 0;
    mc_info->dftl_wmiss = 0;
    // printf("mapping cache size: %d\n\n", ssd->mc_info->cur_size);
#endif
}

/* accept NVMe cmd as input, in order to support more command types in future */
uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    /* TODO: reads need to go through caching layer first */
    /* ... */


    /* on cache miss, read from NAND */
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba; /* sector addr */
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    //struct ssd_channel *ch;
    struct nand_lun *lun;
    bool in_gc = false; /* indicate whether any subIO met GC */

    if(nsecs == 0 || start_lpn > end_lpn)
    {
        return 0;
    }
    if (end_lpn >= spp->tt_pgs) {
        printf("RD-ERRRRRRRRRR,start_lpn=%"PRIu64",end_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }

    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, nsecs);
    //assert(end_lpn < spp->tt_pgs);
    /* for list of NAND page reads involved in this external request, do: */

    req->gcrt = 0;
#define NVME_CMD_GCT (911)
    if (req->tifa_cmd_flag == NVME_CMD_GCT) {
        /* fastfail IO path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }

            //ch = get_ch(ssd, &ppa);
            lun = get_lun(ssd, &ppa);
            if (req->stime < lun->gc_endtime) {
                in_gc = true;
                int tgcrt = lun->gc_endtime - req->stime;
                if (req->gcrt < tgcrt) {
                    req->gcrt = tgcrt;
                }
            } else {
                /* NoGC under fastfail path */
                struct nand_cmd srd;
                srd.type = USER_IO;
                srd.cmd = NAND_READ;
                srd.stime = req->stime;
                sublat = ssd_advance_status(ssd, &ppa, &srd);
                maxlat = (sublat > maxlat) ? sublat : maxlat;
            }
        }

        if (!in_gc) {
            assert(req->gcrt == 0);
            return maxlat;
        }

        assert(req->gcrt > 0);
        if (maxlat > req->gcrt) {
            printf("Coperd,%s,%s,%d,inGC,but qlat(%lu) > gclat(%lu)\n", ssd->ssdname, __func__,
                    __LINE__, maxlat, req->gcrt);
        }
        return 0;
    } else {
        /* normal IO read path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
#if 1
            sublat = ssd_buffer_management(ssd, lpn, req->stime, READ);
#else
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }
            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->stime;
            sublat = ssd_advance_status(ssd, &ppa, &srd);
#endif
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
        /* this is the latency taken by this read request */
        //req->expire_time = maxlat;
        //printf("Coperd,%s,rd,lba:%lu,lat:%lu\n", ssd->ssdname, req->slba, maxlat);
        ssd->cb_info->rdelay += maxlat;
        ssd->cb_info->rcnt++;
        if(ssd->cb_info->rcnt % 100000 == 0)
        {
            printf("avg read delay: %lu\n", ssd->cb_info->rdelay/ssd->cb_info->rcnt);
            ssd->cb_info->rdelay = 0;
            ssd->cb_info->rcnt = 0;
            wk_print(ssd);
            //struct chunk_map_info *ckm_info = ssd->mc_info->ckm_info;
            //double chunk_map_rate = (double)(ckm_info->chunk_map_cnt)/(ckm_info->page_map_cnt + ckm_info->chunk_map_cnt);
            //printf("taichi chunk map rate: %.5f, chunk: %lu, page: %lu, update: %lu\n", chunk_map_rate, ckm_info->chunk_map_cnt, ckm_info->page_map_cnt, ckm_info->update_cnt);
        }
        return maxlat;
    }
}

uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    /* TODO: writes need to go to cache first */
    /* ... */

    if (end_lpn >= spp->tt_pgs) {
        printf("ERRRRRRRRRR,start_lpn=%"PRIu64",end_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
    }
    //assert(end_lpn < spp->tt_pgs);
    //printf("Coperd,%s,end_lpn=%"PRIu64" (%d),len=%d\n", __func__, end_lpn, spp->tt_pgs, len);

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
        //break;
    }

    /* on cache eviction, write to NAND page */

    // are we doing fresh writes ? maptbl[lpn] == FREE, pick a new page
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
#if 1        
        curlat = ssd_buffer_management(ssd, lpn, req->stime, WRITE);
        //ssd_buffer_write(ssd, lpn, req->stime);
#else      
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* overwrite */
            /* update old page information first */
            //printf("Coperd,before-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
            mark_page_invalid(ssd, &ppa);
            //printf("Coperd,after-overwrite,line[%d],ipc=%d,vpc=%d\n", ppa.g.blk, get_line(ssd, &ppa)->ipc, get_line(ssd, &ppa)->vpc);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        /* find a new page */
        ppa = get_new_page(ssd, DATA_PAGE);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd, DATA_PAGE);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
#endif
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    ssd->cb_info->wdelay += maxlat;
    ssd->cb_info->wcnt++;
    if(ssd->cb_info->wcnt % 100000 == 0)
    {
        printf("avg write delay: %lu\n", ssd->cb_info->wdelay/ssd->cb_info->wcnt);
        printf("read cnt: %lu\n", ssd->cb_info->rcnt);
        ssd->cb_info->wdelay = 0;
        ssd->cb_info->wcnt = 0;
        wk_print(ssd);
    }

    return maxlat;
}

