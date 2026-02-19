.intel_syntax noprefix
.code16

.global printf
.global puts
.global putchar
.global exit

/* -----------------------------------------------------------------------------
   exit(int status)
     status: 32-bit integer (stack)
   ----------------------------------------------------------------------------- */
exit:
    push ebp
    mov ebp, esp
    mov eax, [ebp+8]   /* Get status */
    mov ah, 0x4C       /* DOS Function 4Ch: Terminate with return code */
    int 0x21
    /* Unreachable */
    hlt

/* -----------------------------------------------------------------------------
   putchar(int c)
     c: character to print
   ----------------------------------------------------------------------------- */
putchar:
    push ebp
    mov ebp, esp
    mov dl, [ebp+8]    /* Get char */
    mov ah, 0x02       /* DOS Function 02h: Write character to stdout */
    int 0x21
    mov eax, [ebp+8]   /* Return the char written */
    pop ebp
    .byte 0x66, 0xc3

/* -----------------------------------------------------------------------------
   puts(const char *s)
     s: null-terminated string
   ----------------------------------------------------------------------------- */
puts:
    push ebp
    mov ebp, esp
    push esi
    push ebx
    
    mov esi, [ebp+8]   /* Get string pointer */
puts_loop:
    mov al, [esi]
    test al, al
    jz puts_done
    
    /* Print char */
    mov dl, al
    mov ah, 0x02
    int 0x21
    
    inc esi
    jmp puts_loop
    
puts_done:
    /* Print newline */
    mov dl, 13         /* CR */
    mov ah, 0x02
    int 0x21
    mov dl, 10         /* LF */
    mov ah, 0x02
    int 0x21
    
    mov eax, 0         /* Return 0 (success) */
    pop ebx
    pop esi
    pop ebp
    .byte 0x66, 0xc3

/* -----------------------------------------------------------------------------
   printf(const char *format, ...)
     Minimal implementation: %d, %x, %s, %c
   ----------------------------------------------------------------------------- */
printf:
    push ebp
    mov ebp, esp
    push esi
    push ebx
    
    mov esi, [ebp+8]       /* Format string */
    lea ebx, [ebp+12]      /* Argument pointer */
    
printf_loop:
    mov al, [esi]
    test al, al
    jz printf_done
    
    cmp al, '%'
    je printf_format
    
    /* Regular char */
    mov dl, al
    mov ah, 0x02
    int 0x21
    inc esi
    jmp printf_loop
    
printf_format:
    inc esi
    mov al, [esi]
    test al, al
    jz printf_done
    
    cmp al, 'd'
    je printf_dec
    cmp al, 'x'
    je printf_hex
    cmp al, 's'
    je printf_str
    cmp al, 'c'
    je printf_char
    cmp al, '%'
    je printf_percent
    
    /* Unknown format, print percent and char */
    mov dl, '%'
    mov ah, 0x02
    int 0x21
    mov dl, al
    mov ah, 0x02
    int 0x21
    inc esi
    jmp printf_loop

printf_percent:
    mov dl, '%'
    mov ah, 0x02
    int 0x21
    inc esi
    jmp printf_loop

printf_char:
    mov eax, [ebx]
    add ebx, 4
    mov dl, al
    mov ah, 0x02
    int 0x21
    inc esi
    jmp printf_loop

printf_str:
    /* EAX = string pointer from varargs */
    mov eax, [ebx]
    add ebx, 4
    /* Inline print string - no subroutine call needed */
    push esi             /* Save format pointer */
    mov esi, eax
printf_str_loop:
    mov al, [esi]
    test al, al
    jz printf_str_done
    mov dl, al
    mov ah, 0x02
    int 0x21
    inc esi
    jmp printf_str_loop
printf_str_done:
    pop esi              /* Restore format pointer */
    inc esi
    jmp printf_loop

printf_dec:
    /* EAX = integer from varargs */
    mov eax, [ebx]
    add ebx, 4
    /* Inline decimal print */
    push esi             /* Save format pointer */
    push ebx             /* Save arg pointer */
    
    cmp eax, 0
    jge pd_positive
    neg eax
    push eax
    mov dl, '-'
    mov ah, 0x02
    int 0x21
    pop eax
    
pd_positive:
    test eax, eax
    jnz pd_not_zero
    mov dl, '0'
    mov ah, 0x02
    int 0x21
    jmp pd_done

pd_not_zero:
    mov ebx, 10
    mov ecx, 0
pd_loop:
    xor edx, edx
    div ebx
    push edx
    inc ecx
    test eax, eax
    jnz pd_loop
pd_print:
    pop edx
    add dl, '0'
    mov ah, 0x02
    push ecx         /* Save count */
    int 0x21
    pop ecx          /* Restore count */
    dec ecx
    jnz pd_print
    
pd_done:
    pop ebx              /* Restore arg pointer */
    pop esi              /* Restore format pointer */
    inc esi
    jmp printf_loop

printf_hex:
    /* EAX = integer from varargs */
    mov eax, [ebx]
    add ebx, 4
    /* Inline hex print */
    push esi             /* Save format pointer */
    push ebx             /* Save arg pointer */
    
    test eax, eax
    jnz ph_not_zero
    mov dl, '0'
    mov ah, 0x02
    int 0x21
    jmp ph_done

ph_not_zero:
    mov ebx, 16
    mov ecx, 0
ph_loop_div:
    xor edx, edx
    div ebx
    push edx
    inc ecx
    test eax, eax
    jnz ph_loop_div
ph_print_loop:
    pop edx
    cmp dl, 9
    ja ph_alpha
    add dl, '0'
    jmp ph_do_print
ph_alpha:
    add dl, 'a' - 10
ph_do_print:
    mov ah, 0x02
    push ecx         /* Save count */
    int 0x21
    pop ecx          /* Restore count */
    dec ecx
    jnz ph_print_loop
    
ph_done:
    pop ebx              /* Restore arg pointer */
    pop esi              /* Restore format pointer */
    inc esi
    jmp printf_loop

printf_done:
    mov eax, 0
    pop ebx
    pop esi
    pop ebp
    .byte 0x66, 0xc3

/* =============================================================================
   DOS INT 21h syscall wrappers for dos_libc.c
   ============================================================================= */

.global _dos_open
.global _dos_creat
.global _dos_read
.global _dos_write
.global _dos_close
.global _dos_lseek
.global _dos_delete
.global _dos_rename

/* _dos_open(const char *filename, int mode)
   Returns: file handle in EAX, or -1 on error */
_dos_open:
    push ebp
    mov ebp, esp
    push ebx
    
    mov edx, [ebp+8]    /* filename */
    mov eax, [ebp+12]   /* mode: 0=read, 1=write, 2=rw */
    mov ah, 0x3D        /* DOS Open File */
    int 0x21
    jc dos_open_err
    movzx eax, ax
    jmp dos_open_ok
dos_open_err:
    mov eax, -1
dos_open_ok:
    pop ebx
    pop ebp
    .byte 0x66, 0xc3

/* _dos_creat(const char *filename, int attr)
   Returns: file handle in EAX, or -1 on error */
_dos_creat:
    push ebp
    mov ebp, esp
    push ebx
    
    mov edx, [ebp+8]    /* filename */
    mov ecx, [ebp+12]   /* attributes */
    mov ah, 0x3C        /* DOS Create File */
    int 0x21
    jc dos_creat_err
    movzx eax, ax
    jmp dos_creat_ok
dos_creat_err:
    mov eax, -1
dos_creat_ok:
    pop ebx
    pop ebp
    .byte 0x66, 0xc3

/* _dos_read(int handle, void *buf, int count)
   Returns: bytes read in EAX, or -1 on error */
_dos_read:
    push ebp
    mov ebp, esp
    push ebx
    
    mov ebx, [ebp+8]    /* handle */
    mov edx, [ebp+12]   /* buffer */
    mov ecx, [ebp+16]   /* count */
    mov ah, 0x3F        /* DOS Read File */
    int 0x21
    jc dos_read_err
    movzx eax, ax
    jmp dos_read_ok
dos_read_err:
    mov eax, -1
dos_read_ok:
    pop ebx
    pop ebp
    .byte 0x66, 0xc3

/* _dos_write(int handle, const void *buf, int count)
   Returns: bytes written in EAX, or -1 on error */
_dos_write:
    push ebp
    mov ebp, esp
    push ebx
    
    mov ebx, [ebp+8]    /* handle */
    mov edx, [ebp+12]   /* buffer */
    mov ecx, [ebp+16]   /* count */
    mov ah, 0x40        /* DOS Write File */
    int 0x21
    jc dos_write_err
    movzx eax, ax
    jmp dos_write_ok
dos_write_err:
    mov eax, -1
dos_write_ok:
    pop ebx
    pop ebp
    .byte 0x66, 0xc3

/* _dos_close(int handle)
   Returns: 0 on success, -1 on error */
_dos_close:
    push ebp
    mov ebp, esp
    push ebx
    
    mov ebx, [ebp+8]    /* handle */
    mov ah, 0x3E        /* DOS Close File */
    int 0x21
    jnc dos_close_ok
    mov eax, -1
    jmp dos_close_ret
dos_close_ok:
    mov eax, 0
dos_close_ret:
    pop ebx
    pop ebp
    .byte 0x66, 0xc3

/* _dos_lseek(int handle, long offset, int origin)
   Returns: new position in EAX, or -1 on error */
_dos_lseek:
    push ebp
    mov ebp, esp
    push ebx
    
    mov ebx, [ebp+8]    /* handle */
    mov edx, [ebp+12]   /* offset low word */
    mov ecx, 0          /* offset high word = 0 */
    mov eax, [ebp+16]   /* origin */
    mov ah, 0x42        /* DOS Seek File */
    int 0x21
    jc dos_lseek_err
    shl edx, 16
    mov dx, ax
    mov eax, edx
    jmp dos_lseek_ok
dos_lseek_err:
    mov eax, -1
dos_lseek_ok:
    pop ebx
    pop ebp
    .byte 0x66, 0xc3

/* _dos_delete(const char *filename) */
_dos_delete:
    push ebp
    mov ebp, esp
    
    mov edx, [ebp+8]    /* filename */
    mov ah, 0x41        /* DOS Delete File */
    int 0x21
    jnc dos_del_ok
    mov eax, -1
    jmp dos_del_ret
dos_del_ok:
    mov eax, 0
dos_del_ret:
    pop ebp
    .byte 0x66, 0xc3

/* _dos_rename(const char *oldname, const char *newname) */
_dos_rename:
    push ebp
    mov ebp, esp
    push edi
    
    mov edx, [ebp+8]    /* old name */
    mov edi, [ebp+12]   /* new name */
    mov ah, 0x56        /* DOS Rename File */
    int 0x21
    jnc dos_ren_ok
    mov eax, -1
    jmp dos_ren_ret
dos_ren_ok:
    mov eax, 0
dos_ren_ret:
    pop edi
    pop ebp
    .byte 0x66, 0xc3
