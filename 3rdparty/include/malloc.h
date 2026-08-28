#if defined(__APPLE__)
/* macos-port: macOS has no <malloc.h>; malloc/free/realloc/alloca come from
 * <stdlib.h> (and <alloca.h>). This shim sits on the -I path so the ~11 engine
 * TUs that include <malloc.h> compile unchanged. */
#include <stdlib.h>
#include <alloca.h>
#else
/* Windows/Linux: use the real <malloc.h>. */
#include_next <malloc.h>
#endif
