#include <stddef.h>
#include <string.h>

extern "C" {
	void* memcpy_fast(void *destination, const void *source, size_t size);
}

void* memcpyfast(void *, const void *, size_t);

void* memcpyfast(void *to, const void *from, size_t len) {

        if ((char *)to == (char *)from)
                return to;

        if ((char *)to > (char *)from) {
                if ((char *)from + len > (char *)to){
                        return memmove(to, from, len);
                }
        } else {
                if ((char *)to + len > (char *)from){
                        return memmove(to, from, len);
                }
        }

        return memcpy_fast(to, from, len);

}

