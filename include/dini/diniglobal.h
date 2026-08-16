#ifndef DINI_DINIGLOBAL_H
#define DINI_DINIGLOBAL_H

#include <stdcorelib/stdc_global.h>

#ifndef DINI_EXPORT
#  ifdef DINI_STATIC
#    define DINI_EXPORT
#  else
#    ifdef DINI_LIBRARY
#      define DINI_EXPORT STDC_DECL_EXPORT
#    else
#      define DINI_EXPORT STDC_DECL_IMPORT
#    endif
#  endif
#endif

#endif // DINI_DINIGLOBAL_H
