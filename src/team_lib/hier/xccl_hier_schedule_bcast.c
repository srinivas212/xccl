/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2020.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/
#include "config.h"
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "xccl_hier_schedule.h"
#include "xccl_hier_task_schedule.h"
#include "xccl_hier_team.h"
xccl_status_t xccl_hier_collective_finalize(xccl_tl_coll_req_t *request);

static inline int
find_root_by_rank(int root, sbgp_t *sbgp)
{
    int i;
    int root_id = -1;
    for (i=0; i<sbgp->group_size; i++) {
        if (root == sbgp->rank_map[i]) {
            root_id = i;
            break;
        }
    }
    return root_id;
}

enum {
    ROOT_ID_SOCKET,
    ROOT_ID_NODE,
};

static inline int
find_root_by_id(int root_id, sbgp_t *sbgp, xccl_hier_proc_data_t *proc, int id_type)
{
    xccl_hier_team_t *team = sbgp->hier_team;
    int i;
    int root_idx = -1;
    for (i=0; i<sbgp->group_size; i++) {
        int wrank = xccl_hier_team_rank2ctx(team, sbgp->rank_map[i]);
        int id = (id_type == ROOT_ID_SOCKET ? proc[wrank].socketid :
                  proc[wrank].node_id);
        if (id == root_id) {
            root_idx = i;
            break;
        }
    }
    return root_idx;
}

xccl_status_t build_bcast_schedule(xccl_hier_team_t *team, xccl_coll_op_args_t coll,
                                   xccl_hier_bcast_spec_t spec, coll_schedule_t **sched)
{
    int have_node_leaders_group = (team->sbgps[SBGP_NODE_LEADERS].status == SBGP_ENABLED);
    int have_socket_group = (team->sbgps[SBGP_SOCKET].status == SBGP_ENABLED);
    int have_socket_leaders_group = (team->sbgps[SBGP_SOCKET_LEADERS].status == SBGP_ENABLED);
    int have_node_group           = (team->sbgps[SBGP_NODE].status == SBGP_ENABLED);

    int node_leaders_group_exists = (team->sbgps[SBGP_NODE_LEADERS].status != SBGP_NOT_EXISTS);
    int socket_leaders_group_exists = (team->sbgps[SBGP_SOCKET_LEADERS].status != SBGP_NOT_EXISTS);
    int root = coll.root, c = 0;
    int rank = team->super.params.oob.rank;
    int wroot = xccl_hier_team_rank2ctx(team, root);
    int root_on_local_node = is_rank_on_local_node(root, team);
    int root_on_local_socket = root_on_local_node &&
        is_rank_on_local_socket(root, team);
    xccl_hier_context_t *ctx = ucs_derived_of(team->super.ctx, xccl_hier_context_t);
    coll_schedule_single_dep_t *schedule = (coll_schedule_single_dep_t *)malloc(sizeof(*schedule));
    size_t pipeline_thresh = ctx->bcast_pipeline_thresh;
    schedule->super.super.hier_team = team;
    schedule->super.super.type = XCCL_COLL_SCHED_SINGLE_DEP;
    schedule->super.super.progress = coll_schedule_progress_single_dep;
    schedule->super.super.status = XCCL_INPROGRESS;
    schedule->super.fs = NULL;
    schedule->dep_id = -1;

    if (rank == root) {
        schedule->dep_id = 0;
    }
    coll.alg.set_by_user = 0;
    if (have_node_leaders_group) {
        coll.root = find_root_by_id(ctx->procs[wroot].node_id,
                                    &team->sbgps[SBGP_NODE_LEADERS],
                                    ctx->procs, ROOT_ID_NODE);
        schedule->super.args[c].xccl_coll = coll;
        schedule->super.args[c].pair = team->pairs[spec.pairs.node_leaders];
        if (coll.root != team->sbgps[SBGP_NODE_LEADERS].group_rank) {
            schedule->dep_id = c;
        }

        c++;
    }
    if (have_socket_leaders_group) {
        if (root_on_local_node) {
            coll.root = find_root_by_id(ctx->procs[wroot].socketid,
                                        &team->sbgps[SBGP_SOCKET_LEADERS],
                                        ctx->procs, ROOT_ID_SOCKET);
        } else {
            coll.root = 0;
        }
        schedule->super.args[c].xccl_coll = coll;
        if (spec.use_sm_fanout_get) {
            schedule->super.args[c].xccl_coll.coll_type = XCCL_FANOUT_GET;
            schedule->super.args[c].xccl_coll.get_info.memh   = spec.socket_leaders_memh;
            schedule->super.args[c].xccl_coll.get_info.offset = 0;
            schedule->super.args[c].xccl_coll.get_info.len = coll.buffer_info.len;
            schedule->super.args[c].xccl_coll.get_info.local_buffer = coll.buffer_info.dst_buffer;
        }
        schedule->super.args[c].pair = team->pairs[spec.pairs.socket_leaders];
        if (coll.root != team->sbgps[SBGP_SOCKET_LEADERS].group_rank) {
            schedule->dep_id = c;
        }
        c++;
    }
    if (have_socket_group) {
        if (root_on_local_socket) {
            coll.root = find_root_by_rank(root, &team->sbgps[SBGP_SOCKET]);
        } else {
            coll.root = 0;
        }
        schedule->super.args[c].xccl_coll = coll;
        if (spec.use_sm_fanout_get) {
            schedule->super.args[c].xccl_coll.coll_type = XCCL_FANOUT_GET;
            schedule->super.args[c].xccl_coll.get_info.memh   = spec.socket_memh;
            schedule->super.args[c].xccl_coll.get_info.offset = 0;
            schedule->super.args[c].xccl_coll.get_info.len = coll.buffer_info.len;
            schedule->super.args[c].xccl_coll.get_info.local_buffer = coll.buffer_info.dst_buffer;
        }
        schedule->super.args[c].pair = team->pairs[spec.pairs.socket];
        if (coll.root != team->sbgps[SBGP_SOCKET].group_rank) {
            schedule->dep_id = c;
        }
        c++;
    }

    if (team->no_socket && have_node_group) {
        if (root_on_local_node) {
            coll.root = find_root_by_rank(root, &team->sbgps[SBGP_NODE]);
        } else {
            coll.root = 0;
        }
        schedule->super.args[c].xccl_coll = coll;
        if (spec.use_sm_fanout_get) {
            xccl_hier_error("SM Fanout get bcast is not supported with unbound processes");
            abort();
        }
        schedule->super.args[c].pair = team->pairs[spec.pairs.node];
        if (coll.root != team->sbgps[SBGP_NODE].group_rank) {
            schedule->dep_id = c;
        }
        c++;
    }

    assert(schedule->dep_id >= 0);
    schedule->dep_satisfied = 0;
    schedule->super.n_colls = c;
    schedule->super.n_completed_colls = 0;
    memset(schedule->reqs, 0, sizeof(schedule->reqs));
    (*sched) = &schedule->super.super;

    if (coll.buffer_info.len > pipeline_thresh) {
        make_fragmented_schedule(&schedule->super.super, sched, coll.buffer_info,
                                 pipeline_thresh, 1, ctx->bcast_pipeline_depth);
    }

    return XCCL_OK;
}

xccl_status_t build_bcast_task_schedule(xccl_hier_team_t *team, xccl_coll_op_args_t coll,
                                        xccl_hier_bcast_spec_t spec, xccl_seq_schedule_t **sched)
{
    int have_node_leaders_group     = (team->sbgps[SBGP_NODE_LEADERS].status == SBGP_ENABLED);
    int have_socket_group           = (team->sbgps[SBGP_SOCKET].status == SBGP_ENABLED);
    int have_socket_leaders_group   = (team->sbgps[SBGP_SOCKET_LEADERS].status == SBGP_ENABLED);
    int have_node_group             = (team->sbgps[SBGP_NODE].status == SBGP_ENABLED);
    int node_leaders_group_exists   = (team->sbgps[SBGP_NODE_LEADERS].status != SBGP_NOT_EXISTS);
    int socket_leaders_group_exists = (team->sbgps[SBGP_SOCKET_LEADERS].status != SBGP_NOT_EXISTS);


    // sbgp_type_t top_sbgp            = node_leaders_group_exists ? SBGP_NODE_LEADERS :
    //                                   (socket_leaders_group_exists ? SBGP_SOCKET_LEADERS : SBGP_SOCKET);
    int root                        = coll.root;
    int c                           = 0;
    int rank                        = team->super.params.oob.rank;
    int wroot                       = xccl_hier_team_rank2ctx(team, root);
    int root_on_local_node          = is_rank_on_local_node(root, team);
    int root_on_local_socket        = root_on_local_node &&
                                      is_rank_on_local_socket(root, team);
    xccl_hier_context_t *ctx        = ucs_derived_of(team->super.ctx, xccl_hier_context_t);
    xccl_seq_schedule_t *schedule   = (xccl_seq_schedule_t *)malloc(sizeof(*schedule));
    int dep_id, i;
    if (schedule == NULL) {
        return XCCL_ERR_NO_MEMORY;
    }

    schedule->tasks = (xccl_hier_task_t*)malloc(8*sizeof(xccl_hier_task_t));
    xccl_schedule_init(&schedule->super, team->super.ctx);

    // schedule->super.super.hier_team = team;
    // schedule->super.super.type = XCCL_COLL_SCHED_SINGLE_DEP;
    // schedule->super.super.progress = coll_schedule_progress_single_dep;
    // schedule->super.super.status = XCCL_INPROGRESS;
    // schedule->super.fs = NULL;
    // schedule->dep_id = -1;

    if (rank == root) {
        dep_id = 0;
    }

    coll.alg.set_by_user = 0;
    if (have_node_leaders_group) {
        coll.root = find_root_by_id(ctx->procs[wroot].node_id,
                                    &team->sbgps[SBGP_NODE_LEADERS],
                                    ctx->procs, ROOT_ID_NODE);
        schedule->tasks[c].xccl_coll = coll;
        schedule->tasks[c].pair      = team->pairs[spec.pairs.node_leaders];
        xccl_coll_task_init(&schedule->tasks[c].super);

        if (coll.root != team->sbgps[SBGP_NODE_LEADERS].group_rank) {
            dep_id = c;
        }
        c++;
    }
    if (have_socket_leaders_group) {
        if (root_on_local_node) {
            coll.root = find_root_by_id(ctx->procs[wroot].socketid,
                                        &team->sbgps[SBGP_SOCKET_LEADERS],
                                        ctx->procs, ROOT_ID_SOCKET);
        } else {
            coll.root = 0;
        }
        schedule->tasks[c].xccl_coll = coll;
        if (spec.use_sm_fanout_get) {
            schedule->tasks[c].xccl_coll.coll_type = XCCL_FANOUT_GET;
            schedule->tasks[c].xccl_coll.get_info.memh   = spec.socket_leaders_memh;
            schedule->tasks[c].xccl_coll.get_info.offset = 0;
            schedule->tasks[c].xccl_coll.get_info.len = coll.buffer_info.len;
            schedule->tasks[c].xccl_coll.get_info.local_buffer = coll.buffer_info.dst_buffer;
        }
        schedule->tasks[c].pair = team->pairs[spec.pairs.socket_leaders];
        xccl_coll_task_init(&schedule->tasks[c].super);

        if (coll.root != team->sbgps[SBGP_SOCKET_LEADERS].group_rank) {
            dep_id = c;
        }
        c++;
    }
    if (have_socket_group) {
        if (root_on_local_socket) {
            coll.root = find_root_by_rank(root, &team->sbgps[SBGP_SOCKET]);
        } else {
            coll.root = 0;
        }
        schedule->tasks[c].xccl_coll = coll;
        if (spec.use_sm_fanout_get) {
            schedule->tasks[c].xccl_coll.coll_type = XCCL_FANOUT_GET;
            schedule->tasks[c].xccl_coll.get_info.memh   = spec.socket_memh;
            schedule->tasks[c].xccl_coll.get_info.offset = 0;
            schedule->tasks[c].xccl_coll.get_info.len = coll.buffer_info.len;
            schedule->tasks[c].xccl_coll.get_info.local_buffer = coll.buffer_info.dst_buffer;
        }
        schedule->tasks[c].pair = team->pairs[spec.pairs.socket];
        xccl_coll_task_init(&schedule->tasks[c].super);
        if (coll.root != team->sbgps[SBGP_SOCKET].group_rank) {
            dep_id = c;
        }
        c++;
    }
    if (team->no_socket && have_node_group) {
        if (root_on_local_node) {
            coll.root = find_root_by_rank(root, &team->sbgps[SBGP_NODE]);
        } else {
            coll.root = 0;
        }
        schedule->tasks[c].xccl_coll = coll;
        if (spec.use_sm_fanout_get) {
            xccl_hier_error("SM Fanout get bcast is not supported with unbound processes");
            abort();
        }
        schedule->tasks[c].pair = team->pairs[spec.pairs.node];
        xccl_coll_task_init(&schedule->tasks[c].super);
        if (coll.root != team->sbgps[SBGP_NODE].group_rank) {
            dep_id = c;
        }
        c++;
    }

    for (i = 0; i < c; i++) {
        schedule->tasks[i].super.progress  = hier_task_progress_handler;
        schedule->tasks[i].super.handlers[XCCL_EVENT_COMPLETED] = hier_task_completed_handler;
        schedule->tasks[i].req     = NULL;
        schedule->tasks[i].scratch = NULL;

        if (i != dep_id) {
            xccl_event_manager_subscribe(&schedule->tasks[dep_id].super.em,
                                         XCCL_EVENT_COMPLETED, &schedule->tasks[i].super);
        } else {
            schedule->tasks[i].super.handlers[XCCL_EVENT_SCHEDULE_STARTED] = hier_task_completed_handler;
            xccl_event_manager_subscribe(&schedule->super.super.em, XCCL_EVENT_SCHEDULE_STARTED,
                                        &schedule->tasks[i].super);
        }
        xccl_schedule_add_task(&schedule->super, &schedule->tasks[i].super);
    }
    schedule->dep = dep_id;
    (*sched) = schedule;

    return XCCL_OK;
}

typedef struct schedule_bcast_sm_get {
    coll_schedule_t     super;
    coll_schedule_t    *bcast_sched;
    xccl_coll_op_args_t coll;
    xccl_mem_h          sock_memh;
    xccl_mem_h          sock_leaders_memh;
    int                 sock_root;
    int                 sock_leaders_root;
    int                 step;
    xccl_coll_req_h     reqs[2];
} schedule_bcast_sm_get_t;

static xccl_status_t coll_schedule_progress_bcast_sm_get(coll_schedule_t *sched)
{
    schedule_bcast_sm_get_t *schedule = (schedule_bcast_sm_get_t*)sched;
    xccl_status_t s1 = XCCL_OK, s2 = XCCL_OK;
    switch (schedule->step) {
    case 0:
        if (schedule->sock_memh) {
            s1 = xccl_global_mem_map_test(schedule->sock_memh);
        }
        if (schedule->sock_leaders_memh) {
            s2 = xccl_global_mem_map_test(schedule->sock_leaders_memh);
        }
        if (XCCL_OK == s1 && XCCL_OK == s2) {
            xccl_hier_context_t *ctx =
                ucs_derived_of(schedule->super.hier_team->super.ctx,
                                xccl_hier_context_t);
            xccl_hier_bcast_spec_t spec = {
                .use_sm_fanout_get   = 1,
                .pairs               = {
                    .node_leaders    = ctx->tls[XCCL_TL_HMC].enabled ?
                                       XCCL_HIER_PAIR_NODE_LEADERS_HMC :
                                       XCCL_HIER_PAIR_NODE_LEADERS_UCX,
                    .socket          = XCCL_HIER_PAIR_SOCKET_SHMSEG,
                    .socket_leaders  = XCCL_HIER_PAIR_SOCKET_LEADERS_SHMSEG,
                    .node            = XCCL_HIER_PAIR_NODE_UCX,
                },
                .socket_memh         = schedule->sock_memh,
                .socket_leaders_memh = schedule->sock_leaders_memh,
            };
            build_bcast_schedule(sched->hier_team, schedule->coll, spec,
                                 &schedule->bcast_sched);
            schedule->step = 1;
        }
        break;
    case 1:
        assert(schedule->bcast_sched);
        coll_schedule_progress(schedule->bcast_sched);
        if (schedule->bcast_sched->status == XCCL_OK) {
            xccl_hier_collective_finalize(&schedule->bcast_sched->super);
            schedule->bcast_sched = NULL;
            xccl_hier_team_t *team = schedule->super.hier_team;
            if (schedule->sock_memh) {
                xccl_coll_op_args_t args = {
                    .coll_type = XCCL_FANIN,
                    .root = schedule->sock_root,
                    .alg.set_by_user = 0,
                };
                xccl_collective_init(&args, &schedule->reqs[0],
                                     team->pairs[XCCL_HIER_PAIR_SOCKET_SHMSEG]->team);
                xccl_collective_post(schedule->reqs[0]);
            }
            if (schedule->sock_leaders_memh) {
                xccl_coll_op_args_t args = {
                    .coll_type = XCCL_FANIN,
                    .root = schedule->sock_leaders_root,
                    .alg.set_by_user = 0,
                };
                xccl_collective_init(&args, &schedule->reqs[1],
                                     team->pairs[XCCL_HIER_PAIR_SOCKET_LEADERS_SHMSEG]->team);
                xccl_collective_post(schedule->reqs[1]);
            }
            schedule->step = 2;
        }
        break;
    case 2:
        if (schedule->reqs[0]) {
            if (XCCL_OK == xccl_collective_test(schedule->reqs[0])) {
                xccl_collective_finalize(schedule->reqs[0]);
                schedule->reqs[0] = NULL;
            }
        }
        if (schedule->reqs[1]) {
            if (XCCL_OK == xccl_collective_test(schedule->reqs[1])) {
                xccl_collective_finalize(schedule->reqs[1]);
                schedule->reqs[1] = NULL;
            }
        }
        if (NULL == schedule->reqs[0] && NULL == schedule->reqs[1]) {
            if (schedule->sock_memh) {
                xccl_global_mem_unmap(schedule->sock_memh);
            }
            if (schedule->sock_leaders_memh) {
                xccl_global_mem_unmap(schedule->sock_leaders_memh);
            }
            sched->status = XCCL_OK;
            schedule->step = 3;
        }
        break;
    default:
        break;
    }
    return XCCL_OK;
}

/* Broadcast schedule that utilizes the FANOUT_GET capability of a socket/socket_leaders teams.

   Algorithm:
   - If some rank serves as a root at either SOCKET or SOCKET_LEADERS levels it will broadcast its data
   using FANOUT_GET. In order to achieve that the root memory is "mapped" using xccl_global_mem_map
   (with root field specified). When the corresponding operation is completed all the ranks in the
   group may GET the data from the root rank each time it signals fanout.
   - The progress fn will wait for the completion of schedule->step=0. This will indicate the memory handles
   have been broadcasted. At this point we build the schedule for bcast.
   - Step=1: initialize the broadcast schedule. FANOUT_GET is worth using together with fragmentation. The
   memory is mapped once in the beginning of the whole collective. The fragmented message is being progressed
   until all the data is broadcasted. Upon completion SOCKET and SOCKET_LEADERS teams perform FANIN to the
   team root - this will guarantee safe src buffer re-use at root.
   - Step=2. Wait for FANIN completion. Unmap memory. Done. */
xccl_status_t build_bcast_schedule_sm_get(xccl_hier_team_t *team, coll_schedule_t **sched,
                                          xccl_coll_op_args_t coll)
{
    int have_socket_group = (team->sbgps[SBGP_SOCKET].status == SBGP_ENABLED);
    int have_socket_leaders_group = (team->sbgps[SBGP_SOCKET_LEADERS].status == SBGP_ENABLED);
    int root = coll.root;
    int rank = team->super.params.oob.rank;
    int wroot = xccl_hier_team_rank2ctx(team, root);
    int root_on_local_node = is_rank_on_local_node(root, team);
    int root_on_local_socket = root_on_local_node &&
        is_rank_on_local_socket(root, team);
    xccl_hier_context_t *ctx = ucs_derived_of(team->super.ctx, xccl_hier_context_t);
    schedule_bcast_sm_get_t *schedule = calloc(1, sizeof(*schedule));
    schedule->super.hier_team = team;
    schedule->super.progress = coll_schedule_progress_bcast_sm_get;
    schedule->super.status = XCCL_INPROGRESS;
    schedule->coll = coll;
    int sock_leaders_pair = XCCL_HIER_PAIR_SOCKET_LEADERS_SHMSEG;
    int sock_pair = XCCL_HIER_PAIR_SOCKET_SHMSEG;
    assert(ctx->tls[XCCL_TL_SHMSEG].enabled == 1);
    //TODO check if sock/sock_leaders teams support FANOUT_GET and make this fn generic
    // For now, it is enabled manually when SHMSEG is available
    schedule->step = 0;
    xccl_mem_map_params_t params = {
        .field_mask = XCCL_MEM_MAP_PARAM_FIELD_ADDRESS |
                      XCCL_MEM_MAP_PARAM_FIELD_LENGTH |
                      XCCL_MEM_MAP_PARAM_FIELD_ROOT,
        .address = coll.buffer_info.src_buffer,
        .length  = coll.buffer_info.len,
    };
    if (have_socket_leaders_group) {
        if (root_on_local_node) {
            params.root = ctx->procs[wroot].socketid;
        } else {
            params.root = 0;
        }
        schedule->sock_leaders_root = params.root;
        xccl_global_mem_map_start(team->pairs[sock_leaders_pair]->team,
                                  params, &schedule->sock_leaders_memh);
    }
    if (have_socket_group) {
        if (root_on_local_socket) {
            params.root = find_root_by_rank(root, &team->sbgps[SBGP_SOCKET]);
        } else {
            params.root = 0;
        }
        schedule->sock_root = params.root;
        xccl_global_mem_map_start(team->pairs[sock_pair]->team,
                                  params, &schedule->sock_memh);
    }
    (*sched) = &schedule->super;
    return XCCL_OK;
}
