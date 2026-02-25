#ifndef KIARCH_H
#define KIARCH_H

#include "kstatus.h"
#include "../arch/aur64/aur64.h"

typedef void (*KI_ARCH_ENTRY_ROUTINE)(void);

void KiArchSaveContext(AUR64_CONTEXT *OutContext);
void KiArchLoadContext(const AUR64_CONTEXT *Context);
void KiArchSwitchContext(AUR64_CONTEXT *CurrentContext, const AUR64_CONTEXT *NextContext);
void KiArchInitializeContext(AUR64_CONTEXT *Context, uint64_t StackTop, KI_ARCH_ENTRY_ROUTINE EntryRoutine);
void KiArchEnterUserMode(uint64_t UserEntry, uint64_t UserStackTop, uint64_t Arg0, uint64_t Arg1);
KSTATUS KiArchUserModeSmokeTest(void);

#endif /* KIARCH_H */
