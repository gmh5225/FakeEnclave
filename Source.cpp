#include <fltKernel.h>
#include <ntimage.h>
#include "struct.h"

#define YOUR_APP_NAME "dwm.exe"

#define dprintf(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__)

EXTERN_C
PCCHAR
NTAPI
PsGetProcessImageFileName(IN PEPROCESS Process);

EXTERN_C
PVOID
PsGetProcessSectionBaseAddress(__in PEPROCESS Process);

using fnMiObtainReferencedVadEx = void *(NTAPI *)(void *a1, char a2, int *a3);

__declspec(naked) PVOID GetNtosBase()
{
    _asm {
        mov     rax, qword ptr gs:[18h]
        mov     rcx, [rax+38h]
        mov     rax, 0FFFFFFFFFFFFF000h
        and     rax, [rcx+4h]
        jmp      while_begin
        search_begin:
        add     rax, 0FFFFFFFFFFFFF000h
        while_begin: 
        xor     ecx, ecx
        jmp     search_cmp
        search_next: 
        add     rcx, 1
        cmp     rcx, 0FF9h
        jz      search_begin
        search_cmp:  
        cmp     byte ptr[rax+rcx], 48h
        jnz     search_next
        cmp     byte ptr[rax+rcx+1], 8Dh
        jnz     search_next
        cmp     byte ptr[rax+rcx+2], 1Dh
        jnz     search_next
        cmp     byte ptr[rax+rcx+6], 0FFh
        jnz     search_next
        mov     r8d,[rax+rcx+3]
        lea     edx,[rcx+r8]
        add     edx, eax
        add     edx, 7
        test    edx, 0FFFh
        jnz     search_next
        mov     rdx, 0FFFFFFFF00000000h
        and     rdx, rax
        add     r8d, eax
        lea     eax,[rcx+r8]
        add     eax, 7
        or      rax, rdx
        ret
    }
}

static PUCHAR
FindPattern(PVOID Module, ULONG Size, LPCSTR Pattern, LPCSTR Mask)
{
    auto checkMask = [](PUCHAR Buffer, LPCSTR Pattern, LPCSTR Mask) -> bool {
        for (auto x = Buffer; *Mask; Pattern++, Mask++, x++)
        {
            auto addr = *(UCHAR *)(Pattern);
            if (addr != *x && *Mask != '?')
                return false;
        }

        return true;
    };

    for (auto x = 0; x < Size - strlen(Mask); x++)
    {
        auto addr = (PUCHAR)Module + x;
        if (checkMask(addr, Pattern, Mask))
            return addr;
    }

    return nullptr;
}

static PEPROCESS
FindDWMEprocess(ULONG &OutPid)
{
    OutPid = 0;
    PEPROCESS pEpDWM = nullptr;
    for (ULONG i = 0; i < 0x5000; i += 4)
    {
        PEPROCESS pEp = nullptr;
        auto lStatus = PsLookupProcessByProcessId((HANDLE)i, &pEp);
        if (!NT_SUCCESS(lStatus) || !pEp)
        {
            continue;
        }

        auto pName = PsGetProcessImageFileName(pEp);
        // A more casual code
        if (pName && strstr(pName, YOUR_APP_NAME))
        {
            pEpDWM = pEp;
        }
        ObDereferenceObject(pEp);

        if (pEpDWM)
        {
            OutPid = i;
            break;
        }
    }

    return pEpDWM;
}

EXTERN_C
NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    dprintf("new world!\n");

    PVOID pNtosBase = GetNtosBase();
    dprintf("pNtosBase=%p\n", pNtosBase);

    // 48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 41 C7 00 00 00 00 00 4D 8B E0 65 48 8B
    // ?? ?? 88 01 00 00 44 8B ?? 48 8B ?? ?? ?? ?? B8 00 00 00
    fnMiObtainReferencedVadEx pMiObtainReferencedVadEx = (fnMiObtainReferencedVadEx)FindPattern(
        ((PUCHAR)pNtosBase + 0x1000),
        0x50000,
        "\x48\x89\x5C\x24\x10\x48\x89\x74\x24\x18\x48\x89\x7C\x24\x20\x41\x54\x41\x56\x41\x57\x48\x83\xEC\x20\x41\xC7\x00\x00\x00\x00\x00\x4D\x8B\xE0\x65\x48\x8B\x00\x00\x88\x01\x00\x00\x44\x8B\x00\x48\x8B\x00\x00\x00\x00\xB8\x00\x00\x00",
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx??xxxxxx?xx????xxxx");
    dprintf("pMiObtainReferencedVadEx=%p\n", pMiObtainReferencedVadEx);

    if (!pMiObtainReferencedVadEx)
    {
        dprintf("Error: Not found MiObtainReferencedVadEx!\n");
        return -1;
    }

    ULONG uDWMPID;
    PEPROCESS pEpDWM = FindDWMEprocess(uDWMPID);
    dprintf("pEpDWM=%p, uDWMPID=%d!\n", pEpDWM, uDWMPID);
    if (uDWMPID == 0)
    {
        dprintf("Error: Not found DWM!\n");
        return -2;
    }

    PUCHAR pFirstPage = (PUCHAR)PsGetProcessSectionBaseAddress(pEpDWM) + 0x1000;
    dprintf("pFirstPage=%p!\n", pFirstPage);

    KAPC_STATE ks;
    KeStackAttachProcess(pEpDWM, &ks);

    PETHREAD pCurThread = KeGetCurrentThread();
    short uOldSpecialApcDisable = *(short *)((PUCHAR)pCurThread + SpecialApcDisable_17763_OFFSET);

    *(short *)((PUCHAR)pCurThread + SpecialApcDisable_17763_OFFSET) = 0;

    int ns = 0;
    auto pVAD = (PMMVAD_SHORT_17763)pMiObtainReferencedVadEx(pFirstPage, 2, &ns);

    *(short *)((PUCHAR)pCurThread + SpecialApcDisable_17763_OFFSET) = uOldSpecialApcDisable;

    KeUnstackDetachProcess(&ks);

    dprintf("pVAD=%p\n", pVAD);
    if (pVAD)
    {
        dprintf("pVAD->u.VadFlags.PrivateMemory=%d\n", pVAD->u.VadFlags.PrivateMemory);
        dprintf("pVAD->u.VadFlags.Graphics=%d\n", pVAD->u.VadFlags.Graphics);
        dprintf("pVAD->u.VadFlags.Enclave=%d\n", pVAD->u.VadFlags.Enclave);

        pVAD->u.VadFlags.PrivateMemory = 1;
        pVAD->u.VadFlags.Graphics = 1;
        pVAD->u.VadFlags.Enclave = 1;
        dprintf("fake world!\n");
    }

    return STATUS_VIRUS_INFECTED;
}
