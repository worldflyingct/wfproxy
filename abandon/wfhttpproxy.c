#include <stdio.h>
#include <string.h>

enum STATE {
    METHOD,
    IP,
    PORT,
    PARAMKEY,
    PARAMVALUE
};

unsigned int parsehttpproxyheader (char* oldheader, char* newheader, char* host, unsigned int* host_len, unsigned short* pport, int* isconnect, unsigned int* oldheader_len) {
    if (!(memcmp(oldheader, "GET ", sizeof("GET ") - 1) ||
            memcmp(oldheader, "POST ", sizeof("POST ") - 1) ||
            memcmp(oldheader, "PUSH ", sizeof("PUSH ") - 1) ||
            memcmp(oldheader, "DELETE ", sizeof("DELETE ") - 1) ||
            memcmp(oldheader, "CONNECT ", sizeof("CONNECT ") - 1))) {
        printf("http method is unknown\n");
        return 0;
    }
    unsigned short port;
    unsigned int offsetold = 0;
    unsigned int offsetnew = 0;
    enum STATE state = METHOD;
    for (int i = 0, str_len = strlen(oldheader) ; i < str_len ; i++) {
        switch (state) {
            case METHOD:
                if (oldheader[i] == ' ') {
                    i++;
                    if (i == 8) {
                        *isconnect = 1;
                        memcpy(newheader+offsetnew, oldheader+offsetnew, i);
                        offsetnew = i;
                        offsetold = i;
                    } else {
                        *isconnect = 0;
                        memcpy(newheader+offsetnew, oldheader+offsetnew, i);
                        offsetnew = i;
                        i += 7; // 头部一定为"http://"
                        offsetold = i;
                    }
                    state = IP;
                }
                break;
            case IP:
                if (oldheader[i] == ':' || oldheader[i] == '/') {
                    int len = i - offsetold;
                    memcpy(host, oldheader + offsetold, len);
                    host[len] = '\0';
                    *host_len = len;
                    if (oldheader[i] == '/') {
                        offsetold += len;
                        *pport = 80;
                        state = PARAMVALUE;
                    } else {
                        port = 0;
                        state = PORT;
                    }
                }
                break;
            case PORT:
                if (oldheader[i] == '/' || oldheader[i] == ' ') {
                    *pport = port;
                    state = PARAMVALUE;
                } else {
                    port = 10 * port + oldheader[i] - '0';
                }
                break;
            case PARAMKEY:
                if (oldheader[i] == ':') {
                    int len = i + 1 - offsetold;
                    if (!memcmp(oldheader + offsetold, "Proxy-Connection", 16)) {
                        memcpy(newheader + offsetnew, "Connection:", 11);
                        offsetnew += 11;
                        offsetold += len;
                    } else {
                        memcpy(newheader + offsetnew, oldheader + offsetold, len);
                        offsetnew += len;
                        offsetold += len;
                    }
                    state = PARAMVALUE;
                } else if (oldheader[i] == '\r' && oldheader[i+1] == '\n') {
                    i++;
                    int len = i+1-offsetold;
                    memcpy(newheader+offsetnew, oldheader+offsetold, len);
                    offsetnew += len;
                    offsetold += len;
                    *oldheader_len = offsetold;
                    newheader[offsetnew] = '\0';
                    str_len = 0;
                }
                break;
            case PARAMVALUE:
                if (oldheader[i] == '\r' && oldheader[i+1] == '\n') {
                    i++;
                    int len = i+1-offsetold;
                    memcpy(newheader+offsetnew, oldheader+offsetold, len);
                    offsetnew += len;
                    offsetold += len;
                    state = PARAMKEY;
                }
                break;
        }
    }
    return offsetnew;
}
