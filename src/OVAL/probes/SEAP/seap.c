#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <errno.h>
#include <config.h>
#include "public/sm_alloc.h"
#include "generic/common.h"
#include "_sexp-types.h"
#include "_sexp-parse.h"
#include "_seap-types.h"
#include "_seap-scheme.h"
#include "public/seap.h"
#include "_seap-message.h"
#include "_seap-command.h"
#include "_seap-error.h"
#include "_seap-packet.h"

static void SEAP_CTX_initdefault (SEAP_CTX_t *ctx)
{
        _A(ctx != NULL);
        
        ctx->parser  = NULL /* PARSER(label) */;
        ctx->pflags  = PF_EOFOK;
        ctx->fmt_in  = SEXP_FMT_CANONICAL;
        ctx->fmt_out = SEXP_FMT_CANONICAL;

        /* Initialize descriptor table */
        ctx->sd_table.sd = NULL;
        ctx->sd_table.sdsize = 0;
        bitmap_init (&(ctx->sd_table.bitmap), SEAP_MAX_OPENDESC);
        
        ctx->cmd_c_table = SEAP_cmdtbl_new ();
        return;
}

SEAP_CTX_t *SEAP_CTX_new (void)
{
        SEAP_CTX_t *ctx;
        
        ctx = sm_talloc (SEAP_CTX_t);
        SEAP_CTX_initdefault (ctx);
        
        return (ctx);
}

void SEAP_CTX_init (SEAP_CTX_t *ctx)
{
        _A(ctx != NULL);
        SEAP_CTX_initdefault (ctx);
        return;
}

void SEAP_CTX_free (SEAP_CTX_t *ctx)
{
        _A(ctx != NULL);

        /* TODO: free sd_table */
        bitmap_free (&(ctx->sd_table.bitmap));
        SEAP_cmdtbl_free (ctx->cmd_c_table);
        sm_free (ctx);
        return;
}

int SEAP_connect (SEAP_CTX_t *ctx, const char *uri, uint32_t flags)
{
        SEAP_desc_t  *dsc;
        SEAP_scheme_t scheme;
        size_t schstr_len = 0;
        int sd;
        
        while (uri[schstr_len] != ':') {
                if (uri[schstr_len] == '\0') {
                        errno = EINVAL;
                        return (-1);
                }
                ++schstr_len;
        }

        scheme = SEAP_scheme_search (__schtbl, uri, schstr_len);
        if (scheme == SCH_NONE) {
                /* scheme not found */
                errno = EPROTONOSUPPORT;
                return (-1);
        }

        if (uri[schstr_len + 1] == '/') {
                if (uri[schstr_len + 2] == '/') {
                        ++schstr_len;
                        ++schstr_len;
                } else {
                        errno = EINVAL;
                        return (-1);
                }
        } else {
                errno = EINVAL;
                return (-1);
        }
        
        sd = SEAP_desc_add (&(ctx->sd_table), NULL, scheme, NULL);
        
        if (sd < 0) {
                _D("Can't create/add new SEAP descriptor\n");
                return (-1);
        }

        dsc = SEAP_desc_get (&(ctx->sd_table), sd);
        _A(dsc != NULL);
        
        if (SCH_CONNECT(scheme, dsc, uri + schstr_len + 1, flags) != 0) {
                /* FIXME: delete SEAP descriptor */
                _D("FAIL: errno=%u, %s.\n", errno, strerror (errno));
                return (-1);
        }
        
        return (sd);
}

int SEAP_open (SEAP_CTX_t *ctx, const char *path, uint32_t flags)
{
        errno = EOPNOTSUPP;
        return (-1);
}

int SEAP_openfd (SEAP_CTX_t *ctx, int fd, uint32_t flags)
{
        errno = EOPNOTSUPP;
        return (-1);
}

int SEAP_openfd2 (SEAP_CTX_t *ctx, int ifd, int ofd, uint32_t flags)
{
        SEAP_desc_t *dsc;
        int sd;

        sd = SEAP_desc_add (&(ctx->sd_table), NULL, SCH_GENERIC, NULL);
        
        if (sd < 0) {
                _D("Can't create/add new SEAP descriptor\n");
                return (-1);
        }
        
        dsc = SEAP_desc_get (&(ctx->sd_table), sd);
        _A(dsc != NULL);
        
        if (SCH_OPENFD2(SCH_GENERIC, dsc, ifd, ofd, flags) != 0) {
                _D("FAIL: errno=%u, %s.\n", errno, strerror (errno));
                return (-1);
        }

        return (sd);
}

#if 0
int SEAP_openfp (SEAP_CTX_t *ctx, FILE *fp, uint32_t flags)
{
        errno = EOPNOTSUPP;
        return (-1);
}
#endif /* 0 */

int SEAP_recvsexp (SEAP_CTX_t *ctx, int sd, SEXP_t **sexp)
{
        SEAP_msg_t *msg = NULL;

        if (SEAP_recvmsg (ctx, sd, &msg) == 0) {
                *sexp = msg->sexp;
                msg->sexp = NULL;
                SEAP_msg_free (msg);
                
                return (0);
        } else {
                *sexp = NULL;
                return (-1);
        }
}

static void *__SEAP_cmdexec_worker (void *arg)
{
        SEAP_cmdjob_t *job;
        SEXP_t        *res;

        job = (SEAP_cmdjob_t *)arg;
        res = SEAP_cmd_exec (job->ctx, job->sd,
                             SEAP_EXEC_LOCAL,
                             job->cmd->code, job->cmd->args,
                             SEAP_CMDCLASS_USR, NULL, NULL);
        
        /* send */
        
        return (NULL);
}

static int __SEXP_recvmsg_process_cmd (SEAP_CTX_t *ctx, int sd, SEAP_cmd_t *cmd)
{
        SEXP_t *item, *val;
        size_t i, len;
        int mattrs, err;

        if (ctx->cflags & SEAP_CFLG_THREAD) {
                pthread_t      th;
                pthread_attr_t th_attrs;
                
                SEAP_cmdjob_t *job;
                
                /* Initialize thread stuff */
                pthread_attr_init (&th_attrs);
                pthread_attr_setdetachstate (&th_attrs, PTHREAD_CREATE_DETACHED);
                
                /* Prepare the job */
                job = SEAP_cmdjob_new ();
                job->ctx = ctx;
                job->sd  = sd;
                job->cmd = cmd;
                
                /* Create the worker */
                if (pthread_create (&th, &th_attrs,
                                    &__SEAP_cmdexec_worker, (void *)job) != 0)
                {
                        _D("Can't create worker thread: %u, %s.\n", errno, strerror (errno));
                        SEAP_cmdjob_free (job);
                        pthread_attr_destroy (&th_attrs);

                        return (-1);
                }
                
                pthread_attr_destroy (&th_attrs);
        } else {
                SEXP_t *res, *sexp;
                SEAP_desc_t  *dsc;
                
                if (cmd->flags & SEAP_CMDFLAG_REPLY) {
                        res = SEAP_cmd_exec (ctx, sd, SEAP_EXEC_WQUEUE,
                                             cmd->rid, cmd->args,
                                             SEAP_CMDCLASS_USR, NULL, NULL);
                } else {
                        res = SEAP_cmd_exec (ctx, sd, SEAP_EXEC_LOCAL,
                                             cmd->code, cmd->args,
                                             SEAP_CMDCLASS_USR, NULL, NULL);
                        
                        /* send */
                        dsc = SEAP_desc_get (&(ctx->sd_table), sd);
                        
                        if (dsc == NULL) {
                                protect_errno {
                                        SEXP_free (res);
                                }
                                return (-1);
                        }
                        
                        cmd->rid = cmd->id;
#if defined(HAVE_ATOMIC_FUNCTIONS)
                        cmd->id = __sync_fetch_and_add (&(dsc->next_cid), 1);
#else
                        cmd->id = dsc->next_cid++;
#endif
                        cmd->flags |= SEAP_CMDFLAG_REPLY;
                        cmd->args = res;
                        
                        sexp = SEAP_cmd2sexp (cmd);
                        
                        if (SCH_SENDSEXP(dsc->scheme, dsc, sexp, 0) < 0) {
                                _D("SCH_SENDSEXP: FAIL: %u, %s.\n", errno, strerror (errno));
                                SEXP_free (sexp);
                                return (-1);
                        }

                        SEXP_free (sexp);
                }
        }
        
        return (0);
}

int SEAP_recvmsg (SEAP_CTX_t *ctx, int sd, SEAP_msg_t **seap_msg)
{
        SEAP_packet_t *packet;

        _A(ctx      != NULL);
        _A(seap_msg != NULL);
        
        (*seap_msg) = NULL;
        
        /*
         * Packet loop
         */
        for (;;) {
                if (SEAP_packet_recv (ctx, sd, &packet) != 0) {
                        _D("FAIL: ctx=%p, sd=%d, errno=%u, %s.\n",
                           ctx, sd, errno, strerror (errno));
                        return (-1);
                }
                
                switch (SEAP_packet_gettype (packet)) {
                case SEAP_PACKET_MSG:
                        
                        (*seap_msg) = sm_talloc (SEAP_msg_t);
                        memcpy ((*seap_msg), SEAP_packet_msg (packet), sizeof (SEAP_msg_t));
                        
                        return (0);
                case SEAP_PACKET_CMD:
                        switch (__SEAP_recvmsg_process_cmd (ctx, sd, SEAP_packet_cmd (packet))) {
                        case  0:
                                SEAP_packet_free (packet);
                                continue;
                        default:
                                errno = EDOOFUS;
                                return (-1);
                        }
                case SEAP_PACKET_ERR:
                        switch (__SEAP_recvmsg_process_err ()) {
                        default:
                                errno = EDOOFUS;
                                return (-1);
                        }
                default:
                        abort ();
                }
        }
        
        /* NOTREACHED */
        errno = EDOOFUS;
        return (-1);
}

int SEAP_sendmsg (SEAP_CTX_t *ctx, int sd, SEAP_msg_t *seap_msg)
{
        int ret, err;
        SEAP_packet_t *packet;
        SEAP_msg_t    *msg;
        
        packet = SEAP_packet_new ();
        msg    = (SEAP_msg_t *)SEAP_packet_settype (packet, SEAP_PACKET_MSG);
        
        seap_msg->id = SEAP_desc_genmsgid (&(ctx->sd_table), sd);
        memcpy (msg, seap_msg, sizeof (SEAP_msg_t));
        
        ret = SEAP_packet_send (ctx, sd, packet);
        
        protect_errno {
                SEAP_packet_free (packet);
        }
        
        return (ret);
}

int SEAP_sendsexp (SEAP_CTX_t *ctx, int sd, SEXP_t *sexp)
{
        SEAP_msg_t *msg;
        int ret;

        msg = SEAP_msg_new ();
        msg->sexp = sexp;
        ret = SEAP_sendmsg (ctx, sd, msg);
        SEAP_msg_free (msg);
        
        return (ret);
}

#if 0
int SEAP_sendmsg (SEAP_CTX_t *ctx, int sd, SEAP_msg_t *seap_msg)
{
        SEAP_desc_t *desc;
        SEXP_t *sexp_msg;
        uint32_t msg_id;

        _A(ctx != NULL);
        _A(seap_msg != NULL);
        
        if (sd >= 0 && sd < ctx->sd_table.sdsize) {
                desc = &(ctx->sd_table.sd[sd]);

                /* _A(desc->scheme < (sizeof __schtbl / sizeof (SEAP_schemefn_t))); */
        
                /*
                 * Atomicaly fill the id field.
                 */
                
#if defined(HAVE_ATOMIC_FUNCTIONS)
                seap_msg->id = __sync_fetch_and_add (&(desc->next_id), 1);
#else
                seap_msg->id = desc->next_id++;
#endif           
                
                /* Convert seap_msg into its S-exp representation */
                sexp_msg = __SEAP_msg2sexp (seap_msg);
                if (sexp_msg == NULL) {
                        _D("Can't convert message into S-exp: %u, %s.\n",
                           errno, strerror (errno));
                        return (-1);
                }
                
                puts ("--- MSG ---");
                SEXP_printfa (sexp_msg);
                puts ("\n--- MSG ---");
                
                /*
                 * Send the message using handler associated
                 * with the descriptor.
                 */
                if (SCH_SENDSEXP(desc->scheme, desc, sexp_msg, 0) < 0) {
                        /* FIXME: Free sexp_msg */
                        return (-1);
                }
                
                /* check if everything was sent */
                if (desc->ostate != NULL) {
                        errno = EINPROGRESS;
                        return (-1);
                }
                
                return (0);
        } else {
                errno = EBADF;
                return (-1);
        }
}
#endif

int SEAP_reply (SEAP_CTX_t *ctx, int sd, SEAP_msg_t *rep_msg, SEAP_msg_t *req_msg)
{
        _A(ctx != NULL);
        _A(rep_msg != NULL);
        _A(req_msg != NULL);
        
        SEAP_msgattr_set (rep_msg, "reply-id", SEXP_number_newllu (req_msg->id));
        
        return SEAP_sendmsg (ctx, sd, rep_msg);
}

int __SEAP_senderr (SEAP_CTX_t *ctx, int sd, SEAP_err_t *err, unsigned int type)
{
        SEAP_desc_t *desc;
        SEXP_t *sexp_err;
        
        _A(ctx != NULL);
        _A(err != NULL);

        _A(type == SEAP_ETYPE_USER || type == SEAP_ETYPE_INT);
        
        if (sd < 0 || sd >= ctx->sd_table.sdsize) {
                errno = EBADF;
                return (-1);
        }

        desc = &(ctx->sd_table.sd[sd]);

        /* Convert the err structure into its S-exp representation */
        sexp_err = __SEAP_err2sexp (err, type);
        if (sexp_err == NULL) {
                _D("Can't convert the err structure into S-exp: %u, %s.\n",
                   errno, strerror (errno));
                return (-1);
        }
        
        /*
         * Send the error using handler associated
         * with the descriptor.
         */

        if (SCH_SENDSEXP(desc->scheme, desc, sexp_err, 0) < 0) {
                /* FIXME: Don't free the attached message */
                SEXP_free (sexp_err);
                return (-1);
        }

        /* Check if everything was sent */
        if (desc->ostate != NULL) {
                errno = EINPROGRESS;
                return (-1);
        }
        
        return (0);
}

int SEAP_senderr (SEAP_CTX_t *ctx, int sd, SEAP_err_t *err)
{
        return (__SEAP_senderr (ctx, sd, err, SEAP_ETYPE_USER));
}

int SEAP_replyerr (SEAP_CTX_t *ctx, int sd, SEAP_msg_t *rep_msg, uint32_t e)
{
        SEAP_err_t err;
        
        _A(ctx != NULL);
        _A(rep_msg != NULL);
        
        err.code = e;
        err.id   = rep_msg->id;
        err.data = NULL; /* FIXME: Attach original message */
        
        return (__SEAP_senderr (ctx, sd, &err, SEAP_ETYPE_USER));
}

int SEAP_recverr (SEAP_CTX_t *ctx, int sd, SEAP_err_t **err)
{
        return (-1);
}

int SEAP_recverr_byid (SEAP_CTX_t *ctx, int sd, SEAP_err_t **err, SEAP_msgid_t id)
{
        return (-1);
}

SEXP_t *SEAP_read (SEAP_CTX_t *ctx, int sd)
{
        errno = EOPNOTSUPP;
        return (NULL);
}

int SEAP_write (SEAP_CTX_t *ctx, int sd, SEXP_t *sexp)
{
        errno = EOPNOTSUPP;
        return (-1);
}

int SEAP_close (SEAP_CTX_t *ctx, int sd)
{
        SEAP_desc_t *desc;
        int ret = 0;
        
        _A(ctx != NULL);
        
        if (sd > 0) {
                desc = &(ctx->sd_table.sd[sd]);
                /* _A(desc->scheme < (sizeof __schtbl / sizeof (SEAP_schemefn_t))); */
                
                ret = SCH_CLOSE(desc->scheme, desc, 0); /* TODO: Are flags usable here? */
                
                if (SEAP_desc_del (&(ctx->sd_table), sd) != 0) {
                        /* something very bad happened */
                        _D("SEAP_desc_del failed\n");
                        if (ret > 0)
                                ret = -1;
                }
                
                return (ret);
        } else {
                _D("Negative SEAP descriptor\n");
                errno = EBADF;
                return (-1);
        }
}
