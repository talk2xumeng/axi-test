/*
 * 端口与流初始化：队列、mempool、预填模板、按接收地址导流
 */
#ifndef AXIPERF_PORT_H
#define AXIPERF_PORT_H

#include "common.h"

void port_init(uint16_t pi);
void port_fini(void);

#endif
