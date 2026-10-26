#include "platform_unicode.h"
#include <stdlib.h>
#include <limits.h>
#if defined(_WIN32)
#include <windows.h>
#endif

ptrdiff_t qw_platform_utf8_to_utf16(const char *input, uint16_t *output, size_t capacity)
{
    if (!input) return -1;
#if defined(_WIN32)
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, NULL, 0);
    if (required <= 0) return -1;
    if (!output) return required;
    if (capacity < (size_t)required) return -2;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, (wchar_t *)output, required);
#else
    size_t n = 0; const unsigned char *p = (const unsigned char *)input;
    while (*p) { if (*p < 0x80) n++; else if ((*p & 0xe0)==0xc0 && p[1]) { n++; p++; } else if ((*p & 0xf0)==0xe0 && p[1] && p[2]) { n++; p+=2; } else if ((*p & 0xf8)==0xf0 && p[1] && p[2] && p[3]) { n += 2; p+=3; } else return -1; p++; }
    n++; if (!output) return (ptrdiff_t)n; if (capacity < n) return -2;
    size_t i=0; p=(const unsigned char *)input; while(*p){ uint32_t cp; if(*p<0x80){cp=*p++;} else if((*p&0xe0)==0xc0){cp=(*p++&0x1f)<<6; cp|=*p++&0x3f;} else if((*p&0xf0)==0xe0){cp=(*p++&0xf)<<12; cp|=(*p++&0x3f)<<6; cp|=*p++&0x3f;} else {cp=(*p++&7)<<18; cp|=(*p++&0x3f)<<12; cp|=(*p++&0x3f)<<6; cp|=*p++&0x3f;} if(cp<=0xffff) output[i++]=(uint16_t)cp; else {cp-=0x10000; output[i++]=(uint16_t)(0xd800+(cp>>10)); output[i++]=(uint16_t)(0xdc00+(cp&0x3ff));}} output[i]=0; return (ptrdiff_t)(i+1);
#endif
}

ptrdiff_t qw_platform_utf16_to_utf8(const uint16_t *input, char *output, size_t capacity)
{
    if (!input) return -1;
#if defined(_WIN32)
    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, (const wchar_t *)input, -1, NULL, 0, NULL, NULL);
    if (required <= 0) return -1;
    if (!output) return required;
    if (capacity < (size_t)required) return -2;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, (const wchar_t *)input, -1, output, required, NULL, NULL);
#else
    size_t n=0; for(size_t i=0; input[i]; i++){uint32_t cp=input[i]; if(cp>=0xd800&&cp<=0xdbff){uint16_t lo=input[++i]; if(lo<0xdc00||lo>0xdfff)return -1; cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);} if(cp<0x80)n++; else if(cp<0x800)n+=2; else if(cp<0x10000)n+=3; else n+=4;} n++; if(!output)return (ptrdiff_t)n; if(capacity<n)return -2; size_t j=0; for(size_t i=0;input[i];i++){uint32_t cp=input[i]; if(cp>=0xd800&&cp<=0xdbff){uint16_t lo=input[++i]; if(lo<0xdc00||lo>0xdfff)return -1; cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);} if(cp<0x80)output[j++]=(char)cp; else if(cp<0x800){output[j++]=(char)(0xc0|(cp>>6));output[j++]=(char)(0x80|(cp&63));} else if(cp<0x10000){output[j++]=(char)(0xe0|(cp>>12));output[j++]=(char)(0x80|((cp>>6)&63));output[j++]=(char)(0x80|(cp&63));} else {output[j++]=(char)(0xf0|(cp>>18));output[j++]=(char)(0x80|((cp>>12)&63));output[j++]=(char)(0x80|((cp>>6)&63));output[j++]=(char)(0x80|(cp&63));}} output[j]=0; return (ptrdiff_t)(j+1);
#endif
}

#if defined(_WIN32)
wchar_t *qw_platform_utf8_to_wide(const char *input){ptrdiff_t n=qw_platform_utf8_to_utf16(input,NULL,0); if(n<=0)return NULL; wchar_t *out=(wchar_t*)malloc((size_t)n*sizeof(*out)); if(!out)return NULL; if(qw_platform_utf8_to_utf16(input,(uint16_t*)out,(size_t)n)!=n){free(out);return NULL;}return out;}
char *qw_platform_wide_to_utf8(const wchar_t *input){ptrdiff_t n=qw_platform_utf16_to_utf8((const uint16_t*)input,NULL,0); if(n<=0)return NULL; char *out=(char*)malloc((size_t)n);if(!out)return NULL;if(qw_platform_utf16_to_utf8((const uint16_t*)input,out,(size_t)n)!=n){free(out);return NULL;}return out;}
#endif
