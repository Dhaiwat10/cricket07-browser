# Replacement for the stream "take next block" function at 0x76a520 (cdecl, arg: stream handle h).
# Same logic as the original, but h->count and h->block are read, and h->block written, while holding the
# stream lock (ctx+4, the one the producer at 0x769ddf holds while it bumps the count and sets the block).
.intel_syntax noprefix
.text
start:
    push ecx                          # local: ctx
    push ebx
    mov  ebx, dword ptr [esp + 0xc]   # h
    test ebx, ebx
    push ebp
    je   ret0_nolock
    mov  ebp, dword ptr [ebx]         # ctx
    cmp  dword ptr [ebp], 0x4d525453  # 'STRM'
    mov  dword ptr [esp + 8], ebp
    je   valid
ret0_nolock:
    pop  ebp
    xor  eax, eax
    pop  ebx
    pop  ecx
    ret
valid:
    push esi
    push edi
    add  ebp, 4
    push ebp                          # lock argument, kept on the stack until the unlock
    .byte 0xe8
    .long 0x651d60 - 0x76a520 - (. - start) - 4    # call 0x651d60 (EnterCriticalSection wrapper)
    mov  eax, dword ptr [ebx + 8]     # bytes queued
    test eax, eax
    jne  queued
    mov  dword ptr [esp + 0x1c], eax  # nothing queued: return 0
    jmp  unlock_ret
queued:
    mov  eax, dword ptr [ebx + 0xc]   # current block
    mov  esi, dword ptr [eax + 4]
    and  esi, 0xffffff                # its size
    mov  dword ptr [esp + 0x1c], eax  # returned block (in the argument's slot, as the original)
    mov  dword ptr [eax + 4], esi     # drop its stream tag: consumed
    mov  edi, dword ptr [ebx + 8]
    sub  edi, esi
    mov  dword ptr [ebx + 8], edi
    test edi, edi
    jle  unlock_ret
    mov  edx, dword ptr [ebx + 4]     # find the stream's next block
    lea  ecx, [esi + eax]
    mov  eax, dword ptr [ecx + 4]
    mov  esi, eax
    shl  edx, 0x18
    and  esi, 0xff000000
    cmp  esi, edx
    je   found
next:
    cmp  dword ptr [ecx], -1
    jne  advance
    mov  ecx, dword ptr [esp + 0x14]  # ctx: wrap to the start of the ring
    mov  ecx, dword ptr [ecx + 0x3c]
    jmp  check
advance:
    and  eax, 0xffffff
    add  ecx, eax
check:
    mov  eax, dword ptr [ecx + 4]
    mov  esi, eax
    and  esi, 0xff000000
    cmp  esi, edx
    jne  next
found:
    mov  dword ptr [ebx + 0xc], ecx
unlock_ret:
    .byte 0xe8
    .long 0x651d70 - 0x76a520 - (. - start) - 4    # call 0x651d70 (LeaveCriticalSection wrapper)
    add  esp, 4
    mov  eax, dword ptr [esp + 0x18]
    pop  edi
    pop  esi
    pop  ebp
    pop  ebx
    pop  ecx
    ret
