#include <arpa/nameser.h>
#include <netinet/in.h>
#include <resolv.h>
#include <sys/types.h>

int __res_nquery(res_state state, const char* msg, int len, int klass, unsigned char* answer, int anslen) {
    return res_nquery(state, msg, len, klass, answer, anslen);
}

int __dn_expand(const unsigned char* msg, const unsigned char* eom, const unsigned char* src, char* dst, int dstsiz) {
    return dn_expand(msg, eom, src, dst, dstsiz);
}
