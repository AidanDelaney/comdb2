/*
   Copyright 2015 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#ifndef _OSQLSESSION_H_
#define _OSQLSESSION_H_

#include "comdb2.h"
#include "errstat.h"
#include "comdb2uuid.h"
#include "sqloffload.h"

typedef struct osql_sess osql_sess_t;
typedef struct osql_req osql_req_t;
typedef struct blocksql_tran blocksql_tran_t;
typedef struct osql_uuid_req osql_uuid_req_t;

/* Magic rqid value that means "please use uuid instead" */
#define OSQL_RQID_USE_UUID 1

struct osql_sess {

    /* request part */
    unsigned long long rqid; /* identifies the client request session */
    uuid_t uuid;
    queue_type *que; /* queue of received messages */
    int maxquesz;    /* the maximum entries in the queue  */

    pthread_mutex_t mtx; /* mutex and cond for thread sync */
    pthread_cond_t cond;

    struct ireq *iq; /* iq owning this session -- set to NULL once dispatched */
    struct ireq *iqcopy; /* iq owning this session */
    char *offhost;   /* where is the sql peer of this session, 0 for local */

    char tzname[DB_MAX_TZNAMEDB]; /* tzname used for this request */

    int clients;                 /* number of clients;
                                    prevents freeing rq while reader_thread gets a new reply for
                                    it
                                  */
    pthread_mutex_t clients_mtx; /* mutex for clients */

    int terminate; /* gets set if anything goes wrong w/ the session and we need
                      to abort */
    int dispatched; /* Set when session is dispatched to handle_buf */

    enum OSQL_REQ_TYPE type; /* session version */

    unsigned long long completed; /* set to rqid of the completed rqid */
    uuid_t completed_uuid;
    pthread_mutex_t completed_lock;

    struct errstat
        xerr;        /* error info(zeroed if ok), meaningful if completed=1 */
    time_t last_row; /* mark the last received row, used for poking */

    osql_req_t *req; /* request, i.e osql_req_t */
    osql_uuid_req_t *req_uuid;

    int reqlen;      /* length of request */
    const char *sql; /* if set, pointer to sql string (part of req) */
    int sql_allocd;  /* if set, we need to free sql when destroying */
    char *tag; /* dynamic tag header describing query parameter bindings */
    void *
        tagbuf; /* buffer containing query bind parameter values (described by
                   tag) */
    int tagbuflen; /* size of tagbuf */
    blob_buffer_t blobs[MAXBLOBS];
    int numblobs;

    /* this are set for each session retry */
    time_t start; /* when this started */
    time_t end;   /* when this was complete */
    unsigned long long
        seq; /* count how many ops where received, used for id the packet order;
                would be nice if this was generated by replicant, but this will
                do*/
    time_t initstart; /* when this was first started */
    int retries;      /* how many times this session was retried */

    int queryid;
    char tablename[MAXTABLELEN]; // remember tablename in saveop for reordering
    unsigned long long last_genid; // remember updrec/insrec genid for qblobs
    unsigned long long
        ins_seq; // remember key seq for inserts into ins tmp table
    uint16_t tbl_idx;
    bool last_is_ins : 1; // 1 if processing INSERT, 0 for any other oql type
    bool is_reorder_on : 1;
};

enum {
    SESS_PENDING,
    SESS_DONE_OK,
    SESS_DONE_ERROR_REPEATABLE,
    SESS_DONE_ERROR
};

enum { REQ_OPTION_QUERY_LIMITS = 1 };

/**
 * Terminates an in-use osql session (for which we could potentially
 * receive message from sql thread).
 * It calls osql_remove_session.
 * Returns 0 if success
 *
 * NOTE: it is possible to inline clean a request on master bounce,
 * which starts by unlinking the session first, and freeing bplog afterwards
 */
int osql_close_session(struct ireq *iq, osql_sess_t **sess, int is_linked, const char *func, const char *callfunc, int line);

/**
 * Get the cached sql request
 *
 */
osql_req_t *osql_sess_getreq(osql_sess_t *sess);

/**
 * Get the request id, aka rqid
 *
 */
unsigned long long osql_sess_getrqid(osql_sess_t *sess);

/**
 * Register client
 * Prevent temporary the session destruction
 *
 */
int osql_sess_addclient(osql_sess_t *sess);

/**
 * Unregister client
 *
 */
int osql_sess_remclient(osql_sess_t *sess);

/**
 * Registers the destination for osql session "sess"
 *
 */
void osql_sess_setnode(osql_sess_t *sess, char *host);

/**
 * Mark session duration and reported result.
 *
 */
int osql_sess_set_complete(unsigned long long rqid, uuid_t uuid,
                           osql_sess_t *sess, struct errstat *xerr);

/**
 * Check if there was a delay in receiving rows from
 * replicant, and if so, poke the sql session to detect
 * if this is still in progress
 *
 */
int osql_sess_test_slow(blocksql_tran_t *tran, osql_sess_t *sess);

/**
 * Returns
 * - total time (tottm)
 * - last roundtrip time (rtt)
 * - retries (rtrs)
 *
 */
void osql_sess_getsummary(osql_sess_t *sess, int *tottm, int *rtt, int *rtrs);

/**
 * Log query to the reqlog
 */
void osql_sess_reqlogquery(osql_sess_t *sess, struct reqlogger *reqlog);

/**
 * Checks if a session is complete;
 * Returns:
 * - SESS_DONE_OK, if the session completed successfully
 * - SESS_DONE_ERROR_REPEATABLE, if the session is completed
 *   but finished with an error that allows repeating the request
 * - SESS_DONE_ERROR, if the session completed with an unrecoverable error
 * - SESS_PENDING, otherwise
 *
 * xerr is set to point to session errstat so that blockproc can retrieve
 * individual session error, if any.
 *
 *
 */
int osql_sess_test_complete(osql_sess_t *sess, struct errstat **xerr);

/**
 * Print summary session
 *
 */
int osql_sess_getcrtinfo(void *obj, void *arg);

/**
 * Returns associated blockproc transaction
 *
 */
void *osql_sess_getbptran(osql_sess_t *sess);

/* Lock the session */
int osql_sess_lock(osql_sess_t *sess);

/* Unlock the session */
int osql_sess_unlock(osql_sess_t *sess);

/* Return terminated flag */
int osql_sess_is_terminated(osql_sess_t *sess);

/* Set dispatched flag */
void osql_sess_set_dispatched(osql_sess_t *sess, int dispatched);

/* Get dispatched flag */
int osql_sess_dispatched(osql_sess_t *sess);

/* Lock complete lock */
int osql_sess_lock_complete(osql_sess_t *sess);

/* Unlock complete lock */
int osql_sess_unlock_complete(osql_sess_t *sess);

/**
 * Handles a new op received for session "rqid"
 * It saves the packet in the local bplog
 * Return 0 if success
 * Set found if the session is found or not
 *
 */
int osql_sess_rcvop(unsigned long long rqid, uuid_t uuid, int type, void *data,
                    int datalen, int *found);

/**
 * If the node "arg" machine the provided session
 * "obj", mark the session terminated
 * If "*arg: is 0, "obj" is marked terminated anyway
 *
 */
int osql_session_testterminate(void *obj, void *arg);

/**
 * Creates an sock osql session and add it to the repository
 * It makes possible reception of following log ops, but it
 * has no block processor associated yet.
 * Returns created object if success, NULL otherwise
 *
 */
osql_sess_t *osql_sess_create_sock(const char *sql, int sqlen, char *tzname,
                                   int type, unsigned long long rqid,
                                   uuid_t uuid, char *fromhost, struct ireq *iq,
                                   int *replaced, bool is_reorder_on);

char *osql_sess_tag(osql_sess_t *sess);
void *osql_sess_tagbuf(osql_sess_t *sess);
int osql_sess_tagbuf_len(osql_sess_t *sess);
void osql_sess_set_reqlen(osql_sess_t *sess, int len);
void osql_sess_get_blob_info(osql_sess_t *sess, blob_buffer_t **blobs,
                             int *nblobs);
int osql_sess_reqlen(osql_sess_t *sess);

/**
 * Returns
 * - total time (tottm)
 * - last roundtrip time (rtt)
 * - retries (rtrs)
 *
 */
void osql_sess_getsummary(osql_sess_t *sess, int *tottm, int *rtt, int *rtrs);

int osql_sess_type(osql_sess_t *sess);
int osql_sess_queryid(osql_sess_t *sess);
void osql_sess_getuuid(osql_sess_t *sess, uuid_t uuid);

/**
 * Needed for socksql and bro-s, which creates sessions before
 * iq->bplogs.
 * If we fail to dispatch to a blockprocession thread, we need this function
 * to clear the session from repository and free that leaked memory
 *
 */
void osql_sess_clear_on_error(struct ireq *iq, unsigned long long rqid,
                              uuid_t uuid);

int osql_session_is_sorese(osql_sess_t *sess);
int osql_session_set_ireq(osql_sess_t *sess, struct ireq *iq);
struct ireq *osql_session_get_ireq(osql_sess_t *sess);

/**
 * Terminate a session if the session is not yet completed/dispatched
 * Return 0 if session is successfully terminated,
 *        -1 for errors,
 *        1 otherwise (if session was already processed)
 */
int osql_sess_try_terminate(osql_sess_t *sess);
#endif
