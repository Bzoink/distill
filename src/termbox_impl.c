/* termbox2's implementation, compiled once. It is a vendored single header
   (external/src/termbox2, pinned in CMakeLists.txt) whose warnings are not
   ours to fix, so this translation unit alone is built with them quieted --
   the same arrangement as miniaudio. Its built-in Unicode width tables are
   kept (TB_OPT_LIBC_WCHAR is not defined): libc's are locale-dependent, and
   the shell's output never relies on either -- see tui_screen.h. */
#define TB_IMPL
#include "termbox2.h"
