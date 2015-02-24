;/*****************************************************************************/
; OSasm.s: low-level OS commands, written in assembly                       */
; Runs on LM4F120/TM4C123
; A very simple real time operating system with minimal features.
; Daniel Valvano
; January 29, 2015
;
; This example accompanies the book
;  "Embedded Systems: Real Time Interfacing to ARM Cortex M Microcontrollers",
;  ISBN: 978-1463590154, Jonathan Valvano, copyright (c) 2015
;
;  Programs 4.4 through 4.12, section 4.2
;
;Copyright 2015 by Jonathan W. Valvano, valvano@mail.utexas.edu
;    You may use, edit, run or distribute this file
;    as long as the above copyright notice remains
; THIS SOFTWARE IS PROVIDED "AS IS".  NO WARRANTIES, WHETHER EXPRESS, IMPLIED
; OR STATUTORY, INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF
; MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE.
; VALVANO SHALL NOT, IN ANY CIRCUMSTANCES, BE LIABLE FOR SPECIAL, INCIDENTAL,
; OR CONSEQUENTIAL DAMAGES, FOR ANY REASON WHATSOEVER.
; For more information about my classes, my research, and my books, see
; http://users.ece.utexas.edu/~valvano/
; */

        AREA |.text|, CODE, READONLY, ALIGN=2
        THUMB
        REQUIRE8
        PRESERVE8

        EXTERN  RunPt            ; currently running thread
        EXPORT  OS_DisableInterrupts
        EXPORT  OS_EnableInterrupts
        EXPORT  StartOS
        EXPORT  PendSV_Handler
		;EXPORT  SysTick_Handler


OS_DisableInterrupts
        CPSID   I
        BX      LR


OS_EnableInterrupts
        CPSIE   I
        BX      LR

; Does a context switch on demand
PendSV_Handler
	CPSID   I                  ; 2) Prevent interrupt during switch
    PUSH    {R4-R11}           ; 3) Save remaining regs r4-11
    LDR     R0, =RunPt         ; 4) R0=pointer to RunPt, old thread
    LDR     R1, [R0]           ;    R1 = RunPt
    STR     SP, [R1]           ; 5) Save SP into TCB
	
	; this skips sleeping threads
sleeping					   ; KL & DA pg 189 
	LDR R1, [R1,#4]			   ; R1 = RunPt, [R1,#4] = RunPt->next
	LDR R2, [R1, #16]          ; [R1, #16] = RunPt->SleepCtr
	CMP	R2,	#0                 ; if sleeping skip, go to RunPt->next until SleepCtr == 0
	BNE	sleeping
	; end of our added routine
	
    STR     R1, [R0]           ;    RunPt = R1
    LDR     SP, [R1]           ; 7) new thread SP; SP = RunPt->sp;
    POP     {R4-R11}           ; 8) restore regs r4-11
    CPSIE   I                  ; 9) tasks run with interrupts enabled
    BX      LR                 ; 10) restore R0-R3,R12,LR,PC,PSR




StartOS
    LDR     R0, =RunPt         ; currently running thread
    LDR     R2, [R0]           ; R2 = value of RunPt
    LDR     SP, [R2]           ; new thread SP; SP = RunPt->stackPointer;
    POP     {R4-R11}           ; restore regs r4-11
    POP     {R0-R3}            ; restore regs r0-3
    POP     {R12}
    POP     {LR}               ; discard LR from initial stack
    POP     {LR}               ; start location
    POP     {R1}               ; discard PSR
    CPSIE   I                  ; Enable interrupts at processor level
    BX      LR                 ; start first thread


;sleeping
;	LDR R1, [R1,#4]
;	LDR R2, [R1, #8]
;	CMP	R2,	#0
;	BNE	sleeping


; Does a periodic context switch
;SysTick_Handler
;	CPSID   I                  ; 2) Prevent interrupt during switch
;    PUSH    {R4-R11}           ; 3) Save remaining regs r4-11
;    LDR     R0, =RunPt         ; 4) R0=pointer to RunPt, old thread
;    LDR     R1, [R0]           ;    R1 = RunPt
;    STR     SP, [R1]           ; 5) Save SP into TCB
;    LDR     R1, [R1,#4]        ; 6) R1 = RunPt->next
;    STR     R1, [R0]           ;    RunPt = R1
;    LDR     SP, [R1]           ; 7) new thread SP; SP = RunPt->sp;
;    POP     {R4-R11}           ; 8) restore regs r4-11
;    CPSIE   I                  ; 9) tasks run with interrupts enabled
;    BX      LR                 ; 10) restore R0-R3,R12,LR,PC,PSR

; Spin-Lock counting Semaphore
;OS_Wait ;R0 points to counter
;	LDREX	R1,[R0] ; counter
;	SUBS	R1, #1	; counter - 1
;	ITT		PL		; ok if >= 0
;	STREXPL	R2,R1,[R0]	; try update
;	CMPPL	R2, #0	; succeed?
;	BNE		OS_Wait	; no, try again
;	BX		LR

; Spin-Lock counting Semaphore
;OS_Signal ; R0 points to counter
;	LDREX	R1,	[R0]	; counter
;	ADD 	R1,	#1		; counter + 1
;	STREX	R2, R1, [R0]	; try update
;	CMPPL	R2, #0		; succeed?
;	BNE		OS_Signal	; no, try again
;	BX		LR



; DA 2/18/15 This was derived from Spin-Lock Semaphore
; and might be incorrect, changed subtract to LDR R0,#0
; Binary Semaphore
;OS_bWait ;R0 points to counter
;	LDREX	R1,[R0] ; flag
;	LDR		R1, #0	; flag = 0
;	ITT		PL		; ok if >= 0
;	STREXPL	R2,R1,[R0]	; try update
;	CMPPL	R2, #0	; succeed?
;	BNE		OS_Wait	; no, try again
;	BX		LR

; DA 2/18/15 This was derived from the Spin-Lock Semaphore
; and might be incorrect, I just changed Add R1,#1 to LDR R0,#1
; Binary Semaphore
;OS_bSignal ; R0 points to flag
;	LDREX	R1,	[R0]	; flag
;	LDR 	R1,	#1		; flag = 1
;	STREX	R2, R1, [R0]	; try update
;	CMPPL	R2, #0		; succeed?
;	BNE		OS_Signal	; no, try again
;	BX		LR




    ALIGN
    END
		
