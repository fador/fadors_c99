.intel_syntax noprefix
.code32

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
    ret

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
    ret

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
    mov eax, [ebx]
    add ebx, 4
    push eax
    call print_string_no_nl
    add esp, 4
    inc esi
    jmp printf_loop

printf_dec:
    mov eax, [ebx]
    add ebx, 4
    push eax
    call print_decimal
    add esp, 4
    inc esi
    jmp printf_loop

printf_hex:
    mov eax, [ebx]
    add ebx, 4
    push eax
    call print_hex
    add esp, 4
    inc esi
    jmp printf_loop

printf_done:
    mov eax, 0
    pop ebx
    pop esi
    pop ebp
    ret

/* -----------------------------------------------------------------------------
   Helper: print_string_no_nl(char *s)
   ----------------------------------------------------------------------------- */
print_string_no_nl:
    push ebp
    mov ebp, esp
    push esi
    mov esi, [ebp+8]
ps_loop:
    mov al, [esi]
    test al, al
    jz ps_done
    mov dl, al
    mov ah, 0x02
    int 0x21
    inc esi
    jmp ps_loop
ps_done:
    pop esi
    pop ebp
    ret

/* -----------------------------------------------------------------------------
   Helper: print_decimal(int n)
   ----------------------------------------------------------------------------- */
print_decimal:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    
    mov eax, [ebp+8]
    test eax, eax
    jns pd_positive
    
    /* Negative */
    push eax
    mov dl, '-'
    mov ah, 0x02
    int 0x21
    pop eax
    neg eax
    
pd_positive:
    /* Handle 0 explicitly */
    test eax, eax
    jnz pd_not_zero
    mov dl, '0'
    mov ah, 0x02
    int 0x21
    jmp pd_done

pd_not_zero:
    mov ebx, 10
    mov ecx, 0      /* Digit count */
    
pd_loop:
    xor edx, edx
    div ebx         /* EAX = EAX / 10, EDX = EAX % 10 */
    push edx        /* Push remainder */
    inc ecx
    test eax, eax
    jnz pd_loop
    
pd_print:
    pop edx
    add dl, '0'
    mov ah, 0x02
    int 0x21
    loop pd_print
    
pd_done:
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

/* -----------------------------------------------------------------------------
   Helper: print_hex(unsigned int n)
   ----------------------------------------------------------------------------- */
print_hex:
    push ebp
    mov ebp, esp
    push ebx
    
    mov eax, [ebp+8]
    
    test eax, eax
    jnz ph_not_zero
    mov dl, '0'
    mov ah, 0x02
    int 0x21
    jmp ph_send_ret

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
    int 0x21
    loop ph_print_loop
    
ph_send_ret:
    pop ebx
    pop ebp
    ret
