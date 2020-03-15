#ifndef __WFHTTP_H__
#define __WFHTTP_H__

unsigned int parsehttpproxyheader (char* oldheader, char* newheader, char* host, unsigned int* host_len, unsigned short* pport, int* isconnect, unsigned int* oldheader_len);

#endif
