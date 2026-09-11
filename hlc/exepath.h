#ifndef HLC_EXEPATH_H_
#define HLC_EXEPATH_H_

#include <string>

/* Directory the running executable lives in (queried from the OS, not
 * derived from argv[0], so it's correct regardless of how it was invoked). */
std::string GetExecutableDir();

#endif /* HLC_EXEPATH_H_ */
