/******************************************************************************
 * @brief    �����д���
 *
 * Copyright (c) 2015-2020, <master_roger@sina.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs: 
 * Date           Author      Notes 
 * 2015-06-09     roger.luo   ����
 *                             
 * 2017-07-04     roger.luo   �Ż��ֶηָ��
 * 
 * 2020-07-05     roger.luo   ʹ��cli_obj_t����, ֧�ֶ������Դ����
 * 2020-08-29     roger.luo   ֧��ATָ����������Կ���
 * 2020-02-16     roger.luo   ��������������������
 ******************************************************************************/
#ifndef _CMDLINE_H_
#define _CMDLINE_H_

#include "comdef.h"

#define CLI_MAX_CMD_LEN           256            /*�����г���*/                 
#define CLI_MAX_ARGS              16             /*����������*/
#define CLI_MAX_CMDS              64             /*�����������������*/

/** 
 * @brief     CLI��ΪAT������ʹ��,�ڽ���ƥ��ʱ�Զ�����"AT+",
 */
#define CLI_AT_ENABLE             1              

/*�������� */
#define CLI_CMD_TYPE_EXEC         0              /* ��ִͨ������*/
#define CLI_CMD_TYPE_QUERY        1              /* ��ѯ���� (XXX?)*/
#define CLI_CMD_TYPE_SET          2              /* �豸���� (XXX=YY)*/

struct cli_obj;

/*�������*/
typedef struct {
	char	   *name;		                         /*������*/ 
    /**
     * @brief     ��������,����
     * @params    o      - cli ����
     * @params    argc   - �����������
     * @params    argv   - ���������
     * @return    ����ִ�н��, ����ATָ��, ����trueʱ���Զ���ӦOK,����falseʱ��
     *            ��ӦERROR
     */       
	int        (*handler)(struct cli_obj *o, int argc, char *argv[]);   
    const char *brief;                               /*������*/
}cmd_item_t;

#define __cmd_register(name,handler,brief)\
    USED ANONY_TYPE(const cmd_item_t,__cli_cmd_##handler)\
    SECTION("cli.cmd.1") = {name, handler, brief}
    
/*******************************************************************************
 * @brief     ����ע��
 * @params    name      - ������
 * @params    handler   - ��������
 *            ����:int (*handler)(struct cli_obj *s, int argc, char *argv[]);   
 * @params    brief     - ʹ��˵��
 */                 
#define cmd_register(name,handler,brief)\
    __cmd_register(name,handler,brief)

/*cli �ӿڶ��� -------------------------------------------------------------*/
typedef struct {
    /**
     * @brief ͨ������(����)д�ӿ�
     */        
    unsigned int (*write)(const void *buf, unsigned int len);
    /**
     * @brief ͨ������(����)���ӿ�
     */        
    unsigned int (*read) (void *buf, unsigned int len);
    
    /**
     * @brief ����������(����Ҫ����дNULL,������������ִ��)
     * @retval true - ����ִ��, false - ���Դ�����
     */         
    int (*cmd_guard)(char *cmdline);
    
}cli_port_t;

/*�����ж���*/
typedef struct cli_obj {
    int          (*guard)(char *cmdline);
    unsigned int (*write)(const void *buf, unsigned int len);
    unsigned int (*read) (void *buf, unsigned int len);
    void         (*print)(struct cli_obj *this, const char *fmt, ...); 
    int          (*get_val)(struct cli_obj *this);
    char           recvbuf[CLI_MAX_CMD_LEN + 1];  /* ������ջ�����*/
    unsigned short recvcnt;                       /* �����ճ���*/    
    unsigned       type   : 3;                    /* ��������*/
    unsigned       enable : 1;                    /* CLI ���ؿ���*/ 
    unsigned       echo   : 1;                    /* ��������*/    
}cli_obj_t;

void cli_init(cli_obj_t *obj, const cli_port_t *p);

void cli_enable(cli_obj_t *obj);

void cli_disable (cli_obj_t *obj);

void cli_echo_ctrl (cli_obj_t *obj, int echo);

void cli_exec_cmd(cli_obj_t *obj, const char *cmd);

void cli_process(cli_obj_t *obj);


#endif	/* __CMDLINE_H */
