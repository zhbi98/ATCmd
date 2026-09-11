/******************************************************************************
 * @brief        AT command communication management V2
 *
 * Copyright (c) 2020~2022, <morro_luo@163.com>
 *
 * SPDX-License-Identifier: Apathe-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020-01-02     roger.luo    Initial version
 * 2021-01-20     roger.luo    Add debugging interface, and at the same time 
 *                             solve  the problem of uninitialized linked list 
 *                             leading  to segment fault.
 * 2021-03-03     roger.luo    Fix the problem of frequent print receive timeout 
 *                             caused by not clearing the URC counter.
 * 2022-10-01     roger.luo    Redesign the entire AT framework and release V2.0.
 * 2023-01-09     roger.luo    Optimized the processing logic when multi-line 
 *                             command execution fails.
 * 2023-01-10     roger.luo    Resolve URC message containing ':' character 
 *                             causing parsing failure.
 * 2023-02-23     roger.luo    Added a temporary disable interface for URC to 
 *                             solve the problem of URC matching exception when 
 *                             receiving binary data from the modem.
 * 2023-10-27     roger.luo    Added the function of transparent data transmission.
 * 2025-10-28     roger.luo    Solve the problem that the command terminator' \n' 
 *                             exiting transparent transmission is malformed.
 * 2025-12-20     roger.luo    Optimize AT memory usage statistics.
 ******************************************************************************/
#include "at_chat.h"
#include "at_port.h"
#include "linux_list.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#define AT_DEBUG(ai, fmt, args...)                 \
    do                                             \
    {                                              \
        if (__get_adapter(ai)->debug)              \
            __get_adapter(ai)->debug(fmt, ##args); \
    } while (0)

#if AT_LIST_WORK_COUNT < 2
    #error "AT_LIST_WORK_COUNT cannot be less than 2"
#endif

//Used to identify the validity of a work item.
#define WORK_ITEM_TAG 0x2532

#define AT_IS_TIMEOUT(start, time) (at_get_ms() - (start) > (time))

/**AT work type (corresponding to different state machine polling handler.) */
typedef enum {
    WORK_TYPE_GENERAL = 0,             /* General work */
    WORK_TYPE_SINGLLINE,               /* Singlline command */
    WORK_TYPE_MULTILINE,               /* Multiline command */        
    WORK_TYPE_CMD,                     /* Standard command */    
    WORK_TYPE_CUSTOM,                  /* Custom command */
    WORK_TYPE_BUF,                     /* Buffer */
    WORK_TYPE_MAX
} work_type;

/**
 * @brief AT command execution state.
 */
typedef enum {
    AT_STAT_SEND = 0,
    AT_STAT_RECV,
    AT_STAT_RETRY,
} at_cmd_state;

/**
 * @brief AT receive match mask
 */
#define MATCH_MASK_PREFIX 0x01         
#define MATCH_MASK_SUFFIX 0x02         
#define MATCH_MASK_ERROR  0x04         

/**
 * @brief AT work item object
 */
typedef struct {
    struct list_head  node;            /* list node */
    at_attr_t         attr;            /* AT attributes */
    unsigned int      magic : 16;      /* AT magic*/
    unsigned int      state : 3;       /* State of work */
    unsigned int      type  : 3;       /* Type of work */
    unsigned int      code  : 3;       /* Response code*/
    unsigned int      life  : 6;       /* Life cycle countdown(s)*/
    unsigned int      dirty : 1;       /* Dirty flag*/
    union {
        const void *info;
        at_work_t work;                /* Custom work */
        const char * singlline;        
        const char **multiline;        
        void (*sender)(at_env_t *env); /* Custom sender */
        struct {
            unsigned int bufsize;      
            char buf[0];               
        };
    };
} work_item_t;

/**
 * @brief AT Object infomation.
 */
typedef struct {
    at_obj_t          obj;              /* Inherit at_obj*/   
    at_env_t          env;              /* Public work environment*/
    work_item_t      *cursor;           /* Currently running work*/
    struct list_head  hlist, llist;     /* High and low priority queue*/
    struct list_head *clist;            /* Queue currently in use*/
    unsigned int      timer;            /* General purpose timer*/   
    unsigned int      next_delay;       /* Next cycle delay time*/
    unsigned int      delay_timer;      /* Delay timer*/
    char             *recvbuf;          /* Command response receive buffer*/
    char             *prefix;           /* Point to prefix match*/
    char             *suffix;           /* Point to suffix match*/
#if AT_URC_WARCH_EN    
    const urc_item_t *urc_tbl;
    const urc_item_t *urc_item;         /* The currently matched URC item*/
    char             *urcbuf;           
    unsigned int      urc_timer;        
    unsigned short    urc_bufsize;      
    unsigned short    urc_cnt;          
    unsigned short    urc_target;       /* The target data length of the current URC frame*/
    unsigned short    urc_tbl_size;    
    unsigned short    urc_disable_time;     
#endif    
    unsigned short    list_cnt;         
    unsigned short    recv_bufsize;     
    unsigned short    recv_cnt;         /* Command response receives counter*/
    unsigned short    match_len;        /* Response information matching length*/
    unsigned char     match_mask;       /* Response information matching mask*/
    unsigned          urc_enable: 1;    
    unsigned          urc_match : 1;    
    unsigned          enable    : 1;    /* Enable the work */
    unsigned          disposing : 1;    
    unsigned          err_occur : 1;    
    unsigned          raw_trans : 1;
} at_info_t;

/**
 * @brief  Default command attributes.
 */
static const at_attr_t at_def_attr = {
    .params = NULL,    
    .prefix = NULL,
    .suffix = AT_DEF_RESP_OK,
    .cb     = NULL,
    .timeout= AT_DEF_TIMEOUT,
    .retry  = AT_DEF_RETRY,
    .priority = AT_PRIORITY_LOW
};
/*Private static function declarations------------------------------------*/
static void at_send_line(at_info_t *ai, const char *fmt, va_list args);
static void *at_core_malloc(unsigned int nbytes);
static void  at_core_free(void *ptr);

#if AT_MEM_WATCH_EN 
static unsigned int at_max_mem;          /* Maximum memory used*/
static unsigned int at_cur_mem;          /* Currently used memory*/
#endif

/**
 * @brief  at_obj_t * -> at_info_t *
 */
static inline at_info_t *obj_map(at_obj_t *at)
{
    return (at_info_t *)at;
}


static inline const  at_adapter_t *__get_adapter(at_info_t *ai)
{
    return ai->obj.adap;
}

static inline void at_lock(at_info_t *ai)
{
    if (__get_adapter(ai)->lock != NULL)
        __get_adapter(ai)->lock();
}

static inline void at_unlock(at_info_t *ai)
{
    if (__get_adapter(ai)->unlock != NULL)
        __get_adapter(ai)->unlock();
}

static inline void send_data(at_info_t *at, const void *buf, unsigned int len)
{
    __get_adapter(at)->write(buf, len);
}

/**
 * @brief   Send command with newline.
 */
static void send_cmdline(at_info_t *at, const char *cmd)
{
    int len;
    if (cmd == NULL)
        return;
    len = strlen(cmd);
    __get_adapter(at)->write(cmd, len);
    __get_adapter(at)->write("\r\n", 2);
    AT_DEBUG(at,"->\r\n%s", cmd);
}

/**
 * @brief Formatted print with newline.
 */
static void println(at_env_t *env, const char *cmd, ...)
{
    va_list args;
    va_start(args, cmd);
    at_send_line(obj_map(env->obj), cmd, args);
    va_end(args);
}

static unsigned int get_recv_count(at_env_t *env)
{
    return obj_map(env->obj)->recv_cnt;
}

static char *get_recvbuf(at_env_t *env)
{
    return obj_map(env->obj)->recvbuf;
}

static void recvbuf_clear(at_env_t *env)
{
    obj_map(env->obj)->recvbuf[0] = '\0';
    obj_map(env->obj)->recv_cnt = 0;
}

static char *find_substr(at_env_t *env, const char *str)
{
    return strstr(obj_map(env->obj)->recvbuf, str ? str : "");
}

/**
 * @brief   Indicates whether the current work was abort.
 */
static bool at_isabort(at_env_t *env)
{
    at_info_t *ai = obj_map(env->obj);
    if (ai->cursor == NULL)
        return true;
    return ai->cursor->state == AT_WORK_STAT_ABORT;
}

/**
 * @brief   Indication timeout
 */
static bool at_is_timeout(at_env_t *env, unsigned int ms)
{
    return AT_IS_TIMEOUT(obj_map(env->obj)->timer, ms);
}


/**
 * @brief  Reset the currently working timer.
 */
static void at_reset_timer(at_env_t *env)
{
    obj_map(env->obj)->timer = at_get_ms();
}
/**
 * @brief  Set the next poll wait time for the current work.
 */
static void at_next_wait(struct at_env *env, unsigned int ms)
{
    obj_map(env->obj)->next_delay = ms;
    AT_DEBUG(obj_map(env->obj), "Next wait:%d\r\n", ms);
}

static void update_work_state(work_item_t *wi, at_work_state state, at_resp_code code)
{
    wi->state = state;
    wi->code  = code;
#if AT_WORK_CONTEXT_EN
    at_context_t  *ctx = wi->attr.ctx;
    if (ctx != NULL) {
        ctx->code       = (at_resp_code)wi->code;
        ctx->work_state = (at_work_state)wi->state;
    }
#endif
}

/**
 * @brief   End the currently running work.
 */
static void at_finish(struct at_env *env, at_resp_code code)
{
    work_item_t *it = obj_map(env->obj)->cursor;
    update_work_state(it, AT_WORK_STAT_FINISH, code);
}

/**
 * @brief  AT execution callback handler.
 */
static void do_at_callback(at_info_t *ai, work_item_t *wi, at_resp_code code)
{
    at_response_t r;
    AT_DEBUG(ai, "<-\r\n%s", ai->recvbuf);
    //Exception notification
    if ((code == AT_RESP_ERROR || code == AT_RESP_TIMEOUT) && __get_adapter(ai)->error != NULL) {
        __get_adapter(ai)->error(&r);
        ai->err_occur = 1;
        AT_DEBUG(ai, "AT Respose :%s", code == AT_RESP_TIMEOUT ? "timeout" : "error");
    } else {
        ai->err_occur = 0;
    }
#if AT_WORK_CONTEXT_EN
    at_context_t  *ctx = wi->attr.ctx;
    if (ctx != NULL ) {
        if (ctx->respbuf != NULL/* && ctx->bufsize */) {
            ctx->resplen = ai->recv_cnt >= ctx->bufsize ? ctx->bufsize - 1 : ai->recv_cnt;
            memcpy(ctx->respbuf, ai->recvbuf, ctx->resplen);
        }
    }
#endif
    update_work_state(wi, AT_WORK_STAT_FINISH, code);
    //Submit response data and status.
    if (wi->attr.cb) {
        r.obj     = &ai->obj;
        r.params  = wi->attr.params;
        r.recvbuf = ai->recvbuf;
        r.recvcnt = ai->recv_cnt;
        r.code    = code;        
        r.prefix  = ai->prefix != NULL ? ai->prefix : ai->recvbuf;
        r.suffix  = ai->suffix != NULL ? ai->suffix : ai->recvbuf;
        wi->attr.cb(&r);
    }
}

/**
 * @brief  Create a basic work item.
 */
static work_item_t *work_item_create(int extend_size)
{
    work_item_t *it;
    it = at_core_malloc(sizeof(work_item_t) + extend_size);   
    if (it != NULL)
        memset(it, 0, sizeof(work_item_t) + extend_size);
    return it;
}

/**
 * @brief  Destroy work item.
 * @param  it Pointer to an item to destroy.
 */
static void work_item_destroy(work_item_t *it)
{
    if (it != NULL) {
        it->magic = 0;
        at_core_free(it);
    }
}

/**
 * @brief Destroys all work items in the specified queue.
 */
static void work_item_destroy_all(at_info_t *ai, struct list_head *head)
{
    struct list_head *pos, *n;
    work_item_t *it;
    at_lock(ai);
    list_for_each_safe(pos, n, head) {
        it = list_entry(pos, work_item_t, node);
        list_del(&it->node);
        work_item_destroy(it);
    }
    at_unlock(ai);
}

/**
 * @brief  Work item recycling.
 */
static void work_item_recycle(at_info_t *ai, work_item_t *it)
{
    at_lock(ai);    
    if (ai->list_cnt) 
        ai->list_cnt--; 

    list_del(&it->node);
    work_item_destroy(it);
    at_unlock(ai);
}
/**
 * @brief  Create and initialize a work item.
 * @param  type  Type of work item.
 * @param  attr  Item attributes
 * @param  info  additional information.
 * @param  size  the extended size.
 */
static work_item_t *create_work_item(at_info_t *ai, int type, const at_attr_t *attr, const void *info, int extend_size)
{
    work_item_t *it = work_item_create(extend_size);
    if (it == NULL) {
        AT_DEBUG(ai, "Insufficient memory, list count:%d\r\n", ai->list_cnt);
        return NULL;
    }
    if (ai->list_cnt > AT_LIST_WORK_COUNT) {
        AT_DEBUG(ai, "Work queue full\r\n");
        work_item_destroy(it);
        return NULL;
    }
    if (attr == NULL)
        attr = &at_def_attr;
        
    if (type == WORK_TYPE_CMD || type == WORK_TYPE_BUF) {
        memcpy(it->buf, info, extend_size);
        it->bufsize = extend_size;
    } else {
        it->info = info;
    }        
    it->magic = WORK_ITEM_TAG;
    it->attr = *attr;
    it->type = type;
    it->state = AT_WORK_STAT_READY;
#if AT_WORK_CONTEXT_EN    
    if (attr->ctx) {
        attr->ctx->code = AT_RESP_OK;
        attr->ctx->work_state = AT_WORK_STAT_READY;
    }
#endif    
    return it;    
}

static work_item_t *sumit_work_item(at_info_t *ai, work_item_t *it)
{
    if (it != NULL) {
        at_lock(ai);
        list_add_tail(&it->node, it->attr.priority == AT_PRIORITY_HIGH ? &ai->hlist : &ai->llist);         
        ai->list_cnt++;  //Statistics
        at_unlock(ai);
    }
    return it;
}

/**
 * @brief  Create work and put it in the queue
 * @param  type  work of type
 * @param  attr  AT attributes
 * @param  info  Extra info
 * @param  size  Extra info size
 */
static work_item_t *add_work_item(at_info_t *ai, int type, const at_attr_t *attr, const void *info, int extend_size)
{
    work_item_t *it = create_work_item(ai, type, attr, info, extend_size);
    if (it == NULL)
        return NULL;
    return sumit_work_item(ai, it);
}

/**
 * @brief  Initialize matching info
 */
static void match_info_init(at_info_t *ai, at_attr_t *attr)
{
    ai->prefix = ai->suffix = NULL;
    ai->match_len = 0;
    ai->match_mask = 0;        
    if (attr->prefix == NULL || strlen(attr->prefix) == 0)
        ai->match_mask |= MATCH_MASK_PREFIX;
    if (attr->suffix == NULL || strlen(attr->suffix) == 0)
        ai->match_mask |= MATCH_MASK_SUFFIX;
}

/**
 * @brief Custom work processing 
 */
static int do_work_handler(at_info_t *ai)
{
    work_item_t *i = ai->cursor;
    if (ai->next_delay > 0) {
        if (!AT_IS_TIMEOUT(ai->delay_timer, ai->next_delay))
            return 0;
        ai->next_delay = 0;
    }    
    return ((int (*)(at_env_t * e)) i->work)(&ai->env);
}

/**
 * @brief  Generic commands processing 
 */
static int do_cmd_handler(at_info_t *ai)
{
    work_item_t *wi = ai->cursor;
    at_env_t   *env = &ai->env;
    at_attr_t  *attr = &wi->attr;
    switch (env->state)
    {
    case AT_STAT_SEND: 
        if (wi->type == WORK_TYPE_CUSTOM && wi->sender != NULL) {
            wi->sender(env);
        } else if (wi->type == WORK_TYPE_BUF) {
            __get_adapter(ai)->write(wi->buf, wi->bufsize);
        }  else if (wi->type == WORK_TYPE_SINGLLINE) {
            send_cmdline(ai, wi->singlline);
        } else {
            send_cmdline(ai, wi->buf);
        }
        env->state = AT_STAT_RECV;
        env->reset_timer(env);
        env->recvclr(env);
        match_info_init(ai, attr);        
        break;
    case AT_STAT_RECV: /*Receive information and matching processing.*/
        if (ai->match_len != ai->recv_cnt) {
            ai->match_len = ai->recv_cnt;
            //Matching response content prefix.
            if ( !(ai->match_mask & MATCH_MASK_PREFIX) ) {
                ai->prefix = strstr(ai->recvbuf, attr->prefix ? attr->prefix : "");
                ai->match_mask |= ai->prefix ? MATCH_MASK_PREFIX : 0x00;
            } 
            //Matching response content suffix.
            if ( ai->match_mask & MATCH_MASK_PREFIX ) {
                ai->suffix = strstr(ai->prefix ? ai->prefix : ai->recvbuf, attr->suffix ? attr->suffix : "");
                ai->match_mask |= ai->suffix ? MATCH_MASK_SUFFIX : 0x00;
            }
            ai->match_mask |= strstr(ai->recvbuf, AT_DEF_RESP_ERR) ? MATCH_MASK_ERROR : 0x00;
        }
        if (ai->match_mask & MATCH_MASK_ERROR) {  
			AT_DEBUG(ai, "<-\r\n%s\r\n", ai->recvbuf);
            if (env->i++ >= attr->retry) {
                do_at_callback(ai, wi, AT_RESP_ERROR);
                return true;
            }
            
            env->state = AT_STAT_RETRY; //If the command responds incorrectly, it will wait for a while and try again.
            env->reset_timer(env); 
        }
        if (ai->match_mask & MATCH_MASK_SUFFIX) {
            do_at_callback(ai, wi, AT_RESP_OK);
            return true;
        } else if (env->is_timeout(env, attr->timeout)) {
			AT_DEBUG(ai, "Command response timeout, retry:%d\r\n", env->i);
            if (env->i++ >= attr->retry) {
                do_at_callback(ai, wi, AT_RESP_TIMEOUT);
                return true;
            }            
            env->state = AT_STAT_SEND;            
        }
        break;        
    case AT_STAT_RETRY:
        if (env->is_timeout(env, 100))
            env->state = AT_STAT_SEND; /*Go back to the send state*/
        break;
    default:
        env->state = AT_STAT_SEND;
    }
    return false;
}

/**
 * @brief  Multi-line command sending processing.
 */
static int send_multiline_handler(at_info_t *ai)
{
    work_item_t *wi = ai->cursor;
    at_env_t *env = &ai->env;
    at_attr_t *attr = &wi->attr;
    const char **cmds = wi->multiline;

    switch (env->state)
    {
    case AT_STAT_SEND:
        if (cmds[env->i] == NULL) { /* All commands are sent.*/
            do_at_callback(ai, wi, env->params ? AT_RESP_OK : AT_RESP_ERROR);
            return true;
        }
        send_cmdline(ai, cmds[env->i]);
        env->recvclr(env);         
        env->reset_timer(env);
        env->state = AT_STAT_RECV;
        match_info_init(ai, attr);
        break;
    case AT_STAT_RECV:
        if (find_substr(env, attr->suffix)) {
            env->state = 0;
            env->i++;
            env->j = 0;
            env->params = (void *)true; /*Mark execution status*/ 
            AT_DEBUG(ai, "<-\r\n%s", ai->recvbuf);            
        } else if (find_substr(env, AT_DEF_RESP_ERR)) {
            AT_DEBUG(ai, "<-\r\n%s", ai->recvbuf);
            env->j++;
            AT_DEBUG(ai, "CMD:'%s' failed to executed, retry:%d", cmds[env->i], env->j);
            if (env->j >= attr->retry) {
                env->state = 0;
                env->j = 0;
                env->i++;
            } else {
                env->state = AT_STAT_RETRY;       //After the command responds incorrect, try again after a period of time.
                env->reset_timer(env);                
            }
        } else if (env->is_timeout(env, AT_DEF_TIMEOUT)) {
            do_at_callback(ai, wi, AT_RESP_TIMEOUT);
            return true;
        }
        break;
    case AT_STAT_RETRY:
        if (env->is_timeout(env, 100))
            env->state = AT_STAT_SEND;/*Go back to the send state and resend.*/
        break;
    default:
        env->state = AT_STAT_SEND;
    }
    return 0;
}

/**
 * @brief       Send command line
 */
static void at_send_line(at_info_t *ai, const char *fmt, va_list args)
{
    int len;
    char *cmdline;
    cmdline = at_core_malloc(AT_MAX_CMD_LEN);
    if (cmdline == NULL) {
        AT_DEBUG(ai, "Malloc failed when send...\r\n");
        return;
    }
    len = vsnprintf(cmdline, AT_MAX_CMD_LEN, fmt, args);
    //Clear receive buffer.
    ai->recv_cnt = 0;
    ai->recvbuf[0] = '\0';
    send_data(ai, cmdline, len);
    send_data(ai, "\r\n", 2);
    AT_DEBUG(ai,"->\r\n%s\r\n", cmdline);

    at_core_free(cmdline);
}

#if AT_URC_WARCH_EN

/**
 * @brief   Set the AT urc table.
 */
void at_obj_set_urc(at_obj_t *at, const urc_item_t *tbl, int count)
{
    at_info_t *ai = obj_map(at);
    ai->urc_tbl      = tbl;
    ai->urc_tbl_size = count;
}
/**
 * @brief   Get the urc  recv buffer count.
 */
int at_obj_get_urcbuf_count(at_obj_t *at)
{
    return obj_map(at)->urc_cnt;
}
/**
 * @brief   Temporarily disables the URC matching program
 * @param   enable  Enable/disable the URC matching handler
 * @param   timeout Disabled timeout(ms).
 */
void at_obj_urc_set_enable(at_obj_t *at, int enable, unsigned short timeout)
{
    at_info_t *ai = obj_map(at);
    ai->urc_enable = enable ? 1 : 0;
    if (!enable) {
        ai->urc_timer  = at_get_ms();
        ai->urc_disable_time = timeout;
        //AT_DEBUG(ai, "Disable the URC, time:%d ms\r\n", timeout);
    } else {
        //AT_DEBUG(ai, "Enable the URC\r\n");
    }
}

/**
  * @brief Find a URC handler based on URC receive buffer information.
  */
const urc_item_t *find_urc_item(at_info_t *ai, char *urc_buf, unsigned int size)
{
    const urc_item_t *tbl = ai->urc_tbl;
    int i;
    for (i = 0; i < ai->urc_tbl_size && tbl; i++, tbl++) {
       if (strstr(urc_buf, tbl->prefix ? tbl->prefix : ""))//It will need to be further optimized in the future.
            return tbl;  
    }
    return NULL;
}

static void urc_reset(at_info_t *ai)
{
    ai->urc_target = 0;
    ai->urc_cnt    = 0;
    ai->urc_item   = NULL;
    ai->urc_match  = 0;
	ai->urc_timer = at_get_ms();
}

/**
 * @brief       URC(unsolicited code) handler entry.
 * @param[in]   urc    - URC receive buffer
 * @param[in]   size   - URC receive buffer length
 * @return      none
 */
static void urc_handler_entry(at_info_t *ai, urc_recv_status status, char *urc, unsigned int size)
{
    int remain;
    at_urc_info_t ctx = {status, urc, size};
    if (ai->urc_target > 0)
        AT_DEBUG(ai, "<=\r\n%.5s..\r\n", urc);
    else 
        AT_DEBUG(ai, "<=\r\n%s\r\n", urc);    
    /* Send URC event notification. */
    remain = ai->urc_item ? ai->urc_item->handler(&ctx) : 0;
    if (remain == 0 && (ai->urc_item || ai->cursor == NULL)) {

        urc_reset(ai);
    } else {
        AT_DEBUG(ai,"URC receives %d bytes remaining.\r\n", remain);
        ai->urc_target = ai->urc_cnt + remain;
        ai->urc_match = true;
    }
}

static void urc_timeout_process(at_info_t *ai)
{
    //Receive timeout processing, default (MAX_URC_RECV_TIMEOUT).
    if (ai->urc_cnt > 0 && AT_IS_TIMEOUT(ai->urc_timer, AT_URC_TIMEOUT)) {        
        if (ai->urc_cnt > 2 && ai->urc_item != NULL) {
            ai->urcbuf[ai->urc_cnt] = '\0';
            AT_DEBUG(ai,"urc recv timeout=>%s\r\n", ai->urcbuf);       
            urc_handler_entry(ai, URC_RECV_TIMEOUT, ai->urcbuf, ai->urc_cnt);
                    }
        urc_reset(ai);     
    }  
}

/**
 * @brief       URC receive processing
 * @param[in]   buf  - Receive buffer
 * @return      none
 */
static void urc_recv_process(at_info_t *ai, char *buf, unsigned int size)
{
    char *urc_buf;
    int ch;
    if (ai->urcbuf == NULL)
        return;
    if (size == 0) {
        urc_timeout_process(ai);
        return;
    }
    if (!ai->urc_enable) {
        if (!AT_IS_TIMEOUT(ai->urc_timer, ai->urc_disable_time))
            return;
        ai->urc_enable = 1;
        AT_DEBUG(ai, "Enable the URC match handler\r\n");
    }    
	ai->urc_timer = at_get_ms();
    urc_buf  = ai->urcbuf;
    while (size--) {
        ch = *buf++;
        urc_buf[ai->urc_cnt++] = ch;
        if (ai->urc_cnt >= ai->urc_bufsize) {                   /* Empty directly on overflow */
            urc_reset(ai);
            AT_DEBUG(ai, "Urc buffer full.\r\n");
            continue;
        }
        if (ai->urc_match) {
            if (ai->urc_cnt >= ai->urc_target){
                urc_handler_entry(ai, URC_RECV_OK, urc_buf, ai->urc_cnt);
            }                                                    
            continue;
        }
        if (strchr(AT_URC_END_MARKS, ch) == NULL && ch != '\0')  // Find the URC end mark.
            continue;
        urc_buf[ai->urc_cnt] = '\0';
        if (ai->urc_item == NULL) {                              //Find the corresponding URC handler                 
            ai->urc_item = find_urc_item(ai, urc_buf, ai->urc_cnt);
            if (ai->urc_item == NULL && ch == '\n') {
                if (ai->urc_cnt > 2 && ai->cursor == NULL)       //Unrecognized URC message
                    AT_DEBUG(ai, "%s\r\n", urc_buf);
                urc_reset(ai);
                continue;
            }
        }            
        if (ai->urc_item != NULL && ch == ai->urc_item->endmark)
            urc_handler_entry(ai, URC_RECV_OK, urc_buf, ai->urc_cnt);  
    }
          
}
#endif
/**
 * @brief       Command response data processing
 * @return      none
 */
static void resp_recv_process(at_info_t *ai, const char *buf, unsigned int size)
{
    if (size == 0) return;

    if (ai->recv_cnt + size >= ai->recv_bufsize) //Receive overflow, clear directly.
        ai->recv_cnt = 0;

    memcpy(ai->recvbuf + ai->recv_cnt, buf, size);    
    ai->recv_cnt += size;
    ai->recvbuf[ai->recv_cnt] = '\0';
}

static int (*const work_handler_table[WORK_TYPE_MAX])(at_info_t *) = {
    [WORK_TYPE_GENERAL]   = do_work_handler,
    [WORK_TYPE_SINGLLINE] = do_cmd_handler,
    [WORK_TYPE_MULTILINE] = send_multiline_handler,
    [WORK_TYPE_CMD]       = do_cmd_handler,
    [WORK_TYPE_CUSTOM]    = do_cmd_handler,
    [WORK_TYPE_BUF]       = do_cmd_handler,
};

/**
 * @brief   AT work processing.
 */
static void at_work_process(at_info_t *ai)
{
    at_env_t *env = &ai->env;
    if (ai->cursor == NULL) {
        if (!list_empty(&ai->hlist))
            ai->clist = &ai->hlist;
        else if (!list_empty(&ai->llist))
            ai->clist = &ai->llist;
        else
            return; //No work to do.

        at_lock(ai);
        ai->next_delay = 0;
        env->obj    = (struct at_obj *)ai;
        env->i      = 0;
        env->j      = 0;
        env->state  = 0;
        ai->cursor  = list_first_entry(ai->clist, work_item_t, node);
        env->params = ai->cursor->attr.params;
        env->recvclr(env);
        env->reset_timer(env);
        /*Enter running state*/
        if (ai->cursor->state == AT_WORK_STAT_READY) {            
            update_work_state(ai->cursor, AT_WORK_STAT_RUN, (at_resp_code)ai->cursor->code);
        }
        at_unlock(ai);
    }
    /* When the job execution is complete, put it into the idle work queue */
    if (ai->cursor->state >= AT_WORK_STAT_FINISH || work_handler_table[ai->cursor->type](ai)) {
        //Marked the work as done.
        if (ai->cursor->state == AT_WORK_STAT_RUN) {            
            update_work_state(ai->cursor, AT_WORK_STAT_FINISH, (at_resp_code)ai->cursor->code);
        }            
        //Recycle Processed work item.
        work_item_recycle(ai, ai->cursor);
        ai->cursor = NULL;
    }
}

/**
 * @brief  Create an AT object
 * @param  adap AT interface adapter (AT object only saves its pointer, it must be a global resident object)
 * @return Pointer to a new AT object
 */
at_obj_t *at_obj_create(const at_adapter_t *adap)
{
    at_env_t *e;
    at_info_t *ai = at_core_malloc(sizeof(at_info_t));
    if (ai == NULL)
        return NULL;
    memset(ai, 0, sizeof(at_info_t));
    ai->obj.adap = adap;
    /* Initialize high and low priority queues*/
    INIT_LIST_HEAD(&ai->hlist);
    INIT_LIST_HEAD(&ai->llist);
    //Allocate at least 32 bytes to the buffer
    ai->recv_bufsize = adap->recv_bufsize < 32 ? 32 : adap->recv_bufsize;
    ai->recvbuf      = at_core_malloc(ai->recv_bufsize);
    if (ai->recvbuf == NULL) {
        at_obj_destroy(&ai->obj);
        return NULL;
    }
#if AT_URC_WARCH_EN    
    if (adap->urc_bufsize != 0) {
        ai->urc_bufsize  = adap->urc_bufsize < 32 ? 32 : adap->urc_bufsize;
        ai->urcbuf       = at_core_malloc(ai->urc_bufsize);
        if (ai->urcbuf == NULL) {
            at_obj_destroy(&ai->obj);
            return NULL;
        }         
    }
#endif
    e              = &ai->env;
    ai->recv_cnt   = 0;
    ai->urc_enable = 1;
    ai->enable     = 1;
    //Initialization of public work environment.
    e->is_timeout  = at_is_timeout;
    e->println     = println;
    e->recvbuf     = get_recvbuf;
    e->recvclr     = recvbuf_clear;
    e->recvlen     = get_recv_count;
    e->contains    = find_substr;
    e->disposing   = at_isabort;
    e->finish      = at_finish;
    e->reset_timer = at_reset_timer;
    e->next_wait   = at_next_wait;
    return &ai->obj;
}
/**
 * @brief  Destroy a AT object.
 */
void at_obj_destroy(at_obj_t *obj)
{
    at_info_t *ai = obj_map(obj);

    if (obj == NULL)
        return;

    work_item_destroy_all(ai, &ai->hlist);
    work_item_destroy_all(ai, &ai->llist);

    if (ai->recvbuf != NULL)
        at_core_free(ai->recvbuf);
#if AT_URC_WARCH_EN        
    if (ai->urcbuf != NULL)
        at_core_free(ai->urcbuf);
#endif
    at_core_free(ai);

}

/**
 * @brief   Indicates if the AT object is busy.
 * @return  true - command queue is not empty, busy state
 */
bool at_obj_busy(at_obj_t *at)
{
    return !list_empty(&obj_map(at)->hlist) || !list_empty(&obj_map(at)->llist) 
        || obj_map(at)->urc_cnt != 0;
}

/**
 * @brief   Enable/Disable the AT work
 */
void at_obj_set_enable(at_obj_t *at, int enable)
{
    obj_map(at)->enable = enable ? 1 : 0;
}

/**
 * @brief   Set user data
 */
void at_obj_set_user_data(at_obj_t *at, void *user_data)
{
    at->user_data = user_data;
}

/**
 * @brief   Get user data.
 */
void *at_obj_get_user_data(at_obj_t *at)
{
    return at->user_data;
}

/**
 * @brief   Default attributes initialization.
 *          Default low priority, other reference AT_DEF_XXX definition.
 * @param   attr AT attributes
 */
void at_attr_deinit(at_attr_t *attr)
{
    *attr = at_def_attr;
}

/**
 * @brief   Execute command (with variable argument list)
 * @param   attr AT attributes(NULL to use the default value)
 * @param   cmd  Format the command.
 * @param   va   Variable parameter list
 * @return  Indicates whether the asynchronous work was enqueued successfully
 */
bool at_exec_vcmd(at_obj_t *at, const at_attr_t *attr, const char *cmd, va_list va)
{
    char *buf;
    int len;
    void *workid = NULL;
    buf = at_core_malloc(AT_MAX_CMD_LEN);
    if (buf == NULL) {
        AT_DEBUG(obj_map(at), "No memory when execute vcmd...\r\n");
        return NULL;
    }
    len = vsnprintf(buf, AT_MAX_CMD_LEN, cmd, va);
    if (len > 0) {
        workid = add_work_item(obj_map(at), WORK_TYPE_CMD, attr, buf, len + 1);
    }
    at_core_free(buf);
    return workid != NULL;
}

/**
 * @brief   Execute one command
 * @param   attr AT attributes(NULL to use the default value)
 * @param   cmd  Formatted arguments
 * @param   ...  Variable argument list (same usage as printf)
 * @retval  Indicates whether the asynchronous work was enqueued successfully
 */
bool at_exec_cmd(at_obj_t *at, const at_attr_t *attr, const char *cmd, ...)
{
    bool ret;
    va_list args;
    va_start(args, cmd);
    ret = at_exec_vcmd(at, attr, cmd, args);
    va_end(args);
    return ret;
}

/**
 * @brief   Execute custom command
 * @param   attr AT attributes(NULL to use the default value)
 * @param   sender Command sending handler (such as sending any type of data through the env->obj->adap-write interface)
 * @retval  Indicates whether the asynchronous work was enqueued successfully
 */
bool at_custom_cmd(at_obj_t *at, const at_attr_t *attr, void (*sender)(at_env_t *env))
{
    return add_work_item(obj_map(at), WORK_TYPE_CUSTOM, attr, (const void *)sender, 0) != NULL;
}
/**
 * @brief   Send (binary) data
 * @param   attr AT attributes(NULL to use the default value)
 * @param   databuf Binary data
 * @param   bufsize Binary data length
 * @retval  Indicates whether the asynchronous work was enqueued successfully
 */
bool at_send_data(at_obj_t *at, const at_attr_t *attr, const void *databuf, unsigned int bufsize)
{
    return add_work_item(obj_map(at), WORK_TYPE_BUF, attr, databuf, bufsize) != NULL;
}
/**
 * @brief   Send a single-line command
 * @param   cb        Response callback handler, Fill in NULL if not required
 * @param   timeout   command execution timeout(ms)
 * @param   retry     command retries( >= 0)
 * @param   singlline command
 * @retval  Indicates whether the asynchronous work was enqueued successfully.
 * @note    Only the address is saved, so the 'singlline' can not be a local variable which will be destroyed.
 */
bool at_send_singlline(at_obj_t *at, const at_attr_t *attr, const char *singlline)
{
    return add_work_item(obj_map(at), WORK_TYPE_SINGLLINE, attr, singlline, 0) != NULL;
}

/**
 * @brief   Send multiline commands
 * @param   attr AT attributes(NULL to use the default value)
 * @param   multiline Command table, with the last item ending in NULL.
 * @example :
 *          const char *multiline = {
 *              "AT+XXX",
 *              "AT+XXX",
 *              NULL
 *          };
 *          at_send_multiline(at_dev, NULL, multiline);
 * 
 * @retval  Indicates whether the asynchronous work was enqueued successfully.
 * @note    Only the address is saved, so the array can not be a local variable which will be destroyed.
 */
bool at_send_multiline(at_obj_t *at, const at_attr_t *attr, const char **multiline)
{
    return add_work_item(obj_map(at), WORK_TYPE_MULTILINE, attr, multiline, 0) != NULL;
}

/**
 * @brief   Execute custom work.
 * @param   params User parameter.
 * @param   work   AT custom polling work entry, specific usage ref@at_work_t
 * @retval  Indicates whether the asynchronous work was enqueued successfully.
 */
bool at_do_work(at_obj_t *at, void *params, at_work_t work)
{
    at_attr_t attr;
    at_attr_deinit(&attr);
    attr.params = params;
    return add_work_item(obj_map(at), WORK_TYPE_GENERAL, &attr, (const void *)work, 0) != NULL;
}

/**
 * @brief Abort all AT work
 */
void at_work_abort_all(at_obj_t *at)
{
    struct list_head *pos;
    work_item_t *it;
    at_info_t *ai = obj_map(at);    
    at_lock(ai);
    list_for_each(pos, &ai->hlist) {
        it = list_entry(pos, work_item_t, node);
        update_work_state(it, AT_WORK_STAT_ABORT, AT_RESP_ABORT);
    }
    list_for_each(pos, &ai->llist) {
        it = list_entry(pos, work_item_t, node);
        update_work_state(it, AT_WORK_STAT_ABORT, AT_RESP_ABORT);
    }
    at_unlock(ai);
}

#if AT_MEM_WATCH_EN

static void *at_core_malloc(unsigned int nbytes)
{
    if (nbytes + at_cur_mem > AT_MEM_LIMIT_SIZE) { //The maximum memory limit has been exceeded.
        return NULL;
    }
    unsigned long *mem_info = (unsigned long *)at_malloc(nbytes + sizeof(unsigned long));
    if (mem_info == NULL) {
        return NULL;
    }
    *mem_info = nbytes;
    at_cur_mem += (nbytes + sizeof(unsigned long) ); //Statistics of current memory usage.
    if (at_cur_mem > at_max_mem) { //Record maximum memory usage.
        at_max_mem = at_cur_mem;
    }
    return mem_info + 1;
}

static void  at_core_free(void *ptr)
{
    unsigned long *mem_info = (unsigned long *)ptr;
    unsigned long nbytes;
    if (ptr != NULL) {
        mem_info--;
        nbytes = *mem_info;
        at_cur_mem -= (nbytes + sizeof(unsigned long) );
        at_free(mem_info);
    }
}

/**
 * @brief Get the maximum memory usage.
 */
unsigned int at_max_used_memory(void)
{    
    return at_max_mem;
}

/**
 * @brief Get current memory usage.
 */
unsigned int at_cur_used_memory(void)
{
    return at_cur_mem;
}

#else 

static void *at_core_malloc(unsigned int nbytes)
{
    return at_malloc(nbytes); 
}

static void  at_core_free(void *ptr)
{
    at_free(ptr);
}

#endif

#if AT_WORK_CONTEXT_EN

/**
 * @brief  void * -> work_item_t *
 */
static inline work_item_t *work_item_map(void *work_obj)
{
    return (work_item_t *)work_obj;
}

/**
 * @brief       Indicates whether the AT work is valid.
 */
bool at_work_isvalid(void *work_item)
{
    work_item_t *it = work_item_map(work_item);
    return it != NULL && it->magic == WORK_ITEM_TAG;
}

/**
 * @brief  Initialize a work context
 * @param  ctx - Pointer to 'at_context_t'
 * @param  respbuf Command response buffer, fill in NULL if not required
 * @param  bufsize Buffer size (must be long enough to receive all responses)
 */
void at_context_init(at_context_t *ctx, void *respbuf, unsigned bufsize)
{
    memset(ctx, 0, sizeof(at_context_t));
    ctx->bufsize = bufsize;
    ctx->respbuf = respbuf;
}

/**
 * @brief  Attached to the AT attribute.
 * @param  attr AT attributes
 * @param  ctx  AT context
 */
void at_context_attach(at_attr_t *attr, at_context_t *ctx)
{
    attr->ctx = ctx;
}

/**
 * @brief  Get work running state
 * @param  ctx AT context
 * @return State of work.
 */
at_work_state at_work_get_state(at_context_t *ctx)
{
    return ctx->work_state;
}

bool at_work_is_busy(at_context_t *ctx)
{
    return ctx->work_state == AT_WORK_STAT_RUN || ctx->work_state == AT_WORK_STAT_READY;
}
/**
 * @brief  Indicate whether the work has been finished (then you can call `at_work_get_result` to query the result)
 * @param  ctx AT context
 * @return true - the work finish
 */
bool at_work_is_finish(at_context_t *ctx)
{
    return ctx->work_state > AT_WORK_STAT_RUN;
}

/**
 * @brief  Get work running result
 * @param  ctx AT context
 */
at_resp_code  at_work_get_result(at_context_t *ctx)
{
    return ctx->code;
}

#endif

#if AT_RAW_TRANSPARENT_EN
/**
 * @brief  Data transparent transmission processing.
 */
static void at_raw_trans_process(at_obj_t *obj)
{
    unsigned char rbuf[32];
    int size;
    int i;
    at_info_t *ai = obj_map(obj);
    if (obj->raw_conf == NULL)
        return;
    size = obj->adap->read(rbuf, sizeof(rbuf));
    if (size > 0 ){
        obj->raw_conf->write(rbuf, size);
    }
    size = obj->raw_conf->read(rbuf, sizeof(rbuf));
    if (size > 0) {
        obj->adap->write(rbuf, size);
    } 
    //Exit command detection
    if (obj->raw_conf->exit_cmd != NULL) {
        for (i = 0; i < size; i++) {
            if (ai->recv_cnt >= ai->recv_bufsize)
                ai->recv_cnt = 0;
            ai->recvbuf[ai->recv_cnt] = rbuf[i];
            if (rbuf[i] == '\r' || rbuf[i] == '\n') {
                ai->recvbuf[ai->recv_cnt] = '\0';
                ai->recv_cnt = 0;
                if (strcasecmp(obj->raw_conf->exit_cmd, ai->recvbuf) != 0) {
                    continue;
                }
                if (obj->raw_conf->on_exit) {
                    obj->raw_conf->on_exit();
                }
            } else {
                ai->recv_cnt++;
            }
        }
    }
}

/**
 * @brief  Enter transparent transmission mode.
 * @param  conf The configuration for transparent transmission mode.
 */
void at_raw_transport_enter(at_obj_t *obj, const at_raw_trans_conf_t *conf)
{
    at_info_t *ai = obj_map(obj);
    obj->raw_conf = conf;
    ai->raw_trans = 1;
    ai->recv_cnt  = 0;
}

/**
 * @brief  Exit transparent transmission mode.
 */
void at_raw_transport_exit(at_obj_t *obj)
{
    at_info_t *ai = obj_map(obj);
    ai->raw_trans = 0;
}

#endif

/**
 * @brief  AT work polling processing.
 */
void at_obj_process(at_obj_t *at)
{
    char rbuf[64];
    int read_size;
    register at_info_t *ai = obj_map(at);
#if AT_RAW_TRANSPARENT_EN
    if (ai->raw_trans) {
        at_raw_trans_process(at);
        return;
    }    
#endif    
    read_size = __get_adapter(ai)->read(rbuf, sizeof(rbuf));
#if AT_URC_WARCH_EN
        urc_recv_process(ai, rbuf, read_size);
#endif
    resp_recv_process(ai, rbuf, read_size);
    at_work_process(ai);
}

