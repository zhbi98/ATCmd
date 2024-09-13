/******************************************************************************
 * @brief    �����д���
 *
 * Copyright (c) 2015-2020, <morro_luo@163.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs: 
 * Date           Author     Notes 
* 2015-06-09      roger.luo  ����
*                             
* 2017-07-04      roger.luo  �Ż��ֶηָ��
* 
* 2020-07-05      roger.luo  ʹ��cli_obj_t����, ֧�ֶ������Դ����
 ******************************************************************************/
#include "cli.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>

static const cmd_item_t cmd_tbl_start SECTION("cli.cmd.0") = {0};
static const cmd_item_t cmd_tbl_end SECTION("cli.cmd.4") = {0};    
/**
 * @brief       ��������
 * @param[in]   keyword - ����ؼ���
 * @return      ������
 */ 
static const cmd_item_t *find_cmd(const char *keyword, int n)
{                   
	const cmd_item_t *it;
    for (it = &cmd_tbl_start + 1; it < &cmd_tbl_end; it++) {
        if (!strncasecmp(keyword, it->name, n))
            return it;
    }
	return NULL;
}

/*******************************************************************************
 * @brief      �ַ����ָ�  - ��Դ�ַ������ҳ�������separatorָ���ķָ���
 *                            (��',')���滻���ַ���������'\0'�γ��Ӵ���ͬʱ��list
 *                            ָ���б��е�ÿһ��ָ��ֱ�ָ��һ���Ӵ�
 * @example 
 *             input=> s = "abc,123,456,,fb$"  
 *             separator = ",$"
 *            
 *             output=>s = abc'\0' 123'\0' 456'\0' '\0' fb'\0''\0'
 *             list[0] = "abc"
 *             list[1] = "123"
 *             list[2] = "456"
 *             list[3] = ""
 *             list[4] = "fb"
 *             list[5] = ""
 *
 * @param[in] str             - Դ�ַ���
 * @param[in] separator       - �ָ��ַ��� 
 * @param[in] list            - �ַ���ָ���б�
 * @param[in] len             - �б���
 * @return    listָ���б���������������ʾ�򷵻�6
 ******************************************************************************/  
static size_t strsplit(char *s, const char *separator, char *list[],  size_t len)
{
    size_t count = 0;      
    if (s == NULL || list == NULL || len == 0) 
        return 0;     
        
    list[count++] = s;    
    while(*s && count < len) {       
        if (strchr(separator, *s) != NULL) {
            *s = '\0';                                       
            list[count++] = s + 1;                           /*ָ����һ���Ӵ�*/
        }
        s++;        
    }    
    return count;
}



/**
 * @brief ��ӡһ����ʽ���ַ��������ڿ���̨
 * @retval 
 */
static void cli_print(cli_obj_t *obj, const char *format, ...)
{
	va_list args;
    int len;
	char buf[CLI_MAX_CMD_LEN + CLI_MAX_CMD_LEN / 2];
	va_start (args, format);
	len = vsnprintf (buf, sizeof(buf), format, args);
	va_end (args);
    obj->write(buf, len);    
}


/**
 * @brief       ������
 * @param[in]   line - ������
 * @return      none
 **/
static void process_line(cli_obj_t *obj)
{
    char *argv[CLI_MAX_ARGS];
    int   argc, ret, isat = 0;
    const cmd_item_t *it;
    
    //��������
    if (obj->guard && !obj->guard(obj->recvbuf))
        return;
    
    if (obj->echo) {  //����
        obj->print(obj,"%s\r\n",obj->recvbuf);
    }

    argc = strsplit(obj->recvbuf, ",",argv, CLI_MAX_ARGS);
    
    const char *start, *end;

    if (argv[0] == NULL)
        return;

    if (strcasecmp("AT", argv[0]) == 0) {
        obj->print(obj, "OK\r\n");
        return;
    } 
#if CLI_AT_ENABLE != 0
    isat = strncasecmp(argv[0], "AT+", 3) == 0;
#endif
    
    start= !isat ? argv[0] : argv[0] + 3;

    if ((end = strchr(start, '=')) != NULL) {
        obj->type = CLI_CMD_TYPE_SET;

    } else if ((end = strchr(start, '?')) != NULL) {
        obj->type = CLI_CMD_TYPE_QUERY;

    } else {
        obj->type = CLI_CMD_TYPE_EXEC;
        end = start + strlen(argv[0]);
    }
    if (start == end)
        return;

    if ((it = find_cmd(start, end - start)) == NULL) {
        obj->print(obj, "%s\r\n", isat ? "ERROR" : "");
        return;
    }
    ret = it->handler(obj, argc, argv);
    if (isat) {
        obj->print(obj, "%s\r\n" ,ret ? "OK":"ERROR");
    }
}

/**
 * @brief       ��ȡ����ֵ
 */
static int get_val(struct cli_obj *self)
{
    char *p;
    p = strchr(self->recvbuf, '=');
    if ( p == NULL)
        return 0;
    return atoi(p + 1);
  
}

/**
 * @brief       cli ��ʼ��
 * @param[in]   p - cli�����ӿ�
 * @return      none
 */
void cli_init(cli_obj_t *obj, const cli_port_t *p)
{
    obj->read   = p->read;
    obj->write  = p->write;
    obj->print  = cli_print;
    obj->enable = true;
    obj->get_val = get_val;
    obj->guard   = p->cmd_guard;
}

/**
 * @brief       ����cli����ģʽ(cli��ʱ�Զ������û����������)
 * @param[in]   none
 * @return      none
 **/
void cli_enable(cli_obj_t *obj)
{
    char a;
    obj->enable = true;
    obj->recvcnt = 0;
    while (obj->read(&a, 1) > 0) {}
}

/**
 * @brief       �˳�cli����ģʽ(cli��ʱ���ٴ����û����������)
 * @param[in]   none
 * @return      none
 **/
void cli_disable (cli_obj_t *obj)
{
    obj->enable = false;
}

/**
 * @brief       ���Կ���
 * @param[in]   echo - ���Կ��ؿ���(0/1)
 * @return      none
 **/
void cli_echo_ctrl (cli_obj_t *obj, int echo)
{
    obj->echo = echo;
}

/**
 * @brief       ִ��һ������(����cli�Ƿ�����,����ִ��)
 * @param[in]   none
 * @return      none
 **/
void cli_exec_cmd(cli_obj_t *obj, const char *cmd)
{
    int len = strlen(cmd);
    if (len >= CLI_MAX_CMD_LEN - 1)
        return;
    strcpy(obj->recvbuf, cmd);
    process_line(obj);
}

/**
 * @brief       �����д������
 * @param[in]   none
 * @return      none
 **/
void cli_process(cli_obj_t *obj)
{    
    char buf[32];
    int i, readcnt;
    if (!obj->read || !obj->enable)
        return;
    
    readcnt = obj->read(buf, sizeof(buf));
    
    if (readcnt) {
        for (i = 0; i < readcnt; i++) {
            if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\0') {
                obj->recvbuf[obj->recvcnt] = '\0';
                if (obj->recvcnt > 1)
                    process_line(obj);
                obj->recvcnt = 0;
            } else {
                obj->recvbuf[obj->recvcnt++] = buf[i];
                
                if (obj->recvcnt >= CLI_MAX_CMD_LEN) /*��������֮��ǿ�����*/
                    obj->recvcnt = 0;
            }
        }
    }
}

#if 1
/*******************************************************************************
* @brief	   ����Ƚ���
* @param[in]   none
* @return 	   �ο�strcmp
*******************************************************************************/
static int cmd_item_comparer(const void *item1,const void *item2)
{
    cmd_item_t *it1 = *((cmd_item_t **)item1); 
    cmd_item_t *it2 = *((cmd_item_t **)item2);    
    return strcmp(it1->name, it2->name);
}
#endif

/*
 * @brief	   ��������
 */
static int do_help (struct cli_obj *s, int argc, char *argv[])
{
	int i,j, count;
    const cmd_item_t *item_start = &cmd_tbl_start + 1; 
    const cmd_item_t *item_end   = &cmd_tbl_end;	
	const cmd_item_t *cmdtbl[CLI_MAX_CMDS];
    
    if (argc == 2) {
        if ((item_start = find_cmd(argv[1], strlen(argv[1]))) != NULL) 
        {
            s->print(s, item_start->brief);                  /*����ʹ����Ϣ----*/
            s->print(s, "\r\n");   
        }
        return 0;
    }
    for (i = 0; i < item_end - item_start && i < CLI_MAX_ARGS; i++)
        cmdtbl[i] = &item_start[i];
    count = i;
    /*������������� ---------------------------------------------------------*/
    qsort(cmdtbl, i, sizeof(cmd_item_t*), cmd_item_comparer);    		        
    s->print(s, "\r\n");
    for (i = 0; i < count; i++) {
        s->print(s, cmdtbl[i]->name);                        /*��ӡ������------*/
        /*�������*/
        j = strlen(cmdtbl[i]->name);
        if (j < 10)
            j = 10 - j;
            
        while (j--)
            s->print(s, " ");
            
        s->print(s, "- "); 
		s->print(s, cmdtbl[i]->brief);                       /*����ʹ����Ϣ----*/
        s->print(s, "\r\n");        
    }
	return 1;
}
 /*ע��������� ---------------------------------------------------------------*/
cmd_register("help", do_help, "list all command.");   
cmd_register("?",    do_help, "alias for 'help'");
