
// Authors: Dalton Altstaetter & Ken Lee
// February 3, 2015
// EE445M Spring 2015

#include "OS.h"
#include "tm4c123gh6pm.h"
#include <stdio.h>
#include <stdint.h>
#include "PLL.h"
#include "TIMER.h"
#include "ifdef.h"

// function definitions in osasm.s
void OS_DisableInterrupts(void); // Disable interrupts
void OS_EnableInterrupts(void);  // Enable interrupts
int32_t StartCritical(void);
void EndCritical(int32_t primask);
void PendSV_Handler(); // used for context switching in SysTick
void StartOS(void);

#define MAILBOX_EMPTY	1
#define MAILBOX_FULL	0

#define DATA_VALID 1
#define DATA_NOT_VALID 0

#define FREE 0
#define USED 1

#define NUMTHREADS 12
#define STACKSIZE 128
struct tcb{
	int32_t *sp;
	struct tcb *next;
	struct tcb *previous;
	int32_t ID;
	int32_t SleepCtr;
	int32_t Priority;
	int32_t MemStatus;
};
typedef struct tcb tcbType;
tcbType tcbs[NUMTHREADS];
tcbType *RunPt;
int32_t Stacks[NUMTHREADS][STACKSIZE];
int32_t g_sleepingThreads[NUMTHREADS];
int32_t g_numSleepingThreads = 0;
Sema4Type g_mailboxDataValid, g_mailboxFree;
Sema4Type g_dataAvailable, g_roomLeft, g_fifoMutex;
unsigned long g_msTime; // num of ms since SysTick has started counting



#define FIFOMAXSIZE 128
#define FIFO_SUCCESS 1
#define FIFO_FAIL 0
unsigned long *g_fifoPutPtr, *g_fifoGetPtr;
unsigned long Fifo[FIFOMAXSIZE];
unsigned long* g_Fifo;
unsigned int g_FIFOSIZE;



volatile int mutex;
volatile int RoomLeft;
volatile int CurrentSize;
Sema4Type LCDmutex;
unsigned long g_mailboxData;
unsigned long* g_ulFifo; // pointer to OS_FIFO

void SetInitialStack(int i){
  tcbs[i].sp = &Stacks[i][STACKSIZE-16]; // thread stack pointer
  Stacks[i][STACKSIZE-1] = 0x01000000;   // thumb bit
  Stacks[i][STACKSIZE-3] = 0x14141414;   // R14
  Stacks[i][STACKSIZE-4] = 0x12121212;   // R12
  Stacks[i][STACKSIZE-5] = 0x03030303;   // R3
  Stacks[i][STACKSIZE-6] = 0x02020202;   // R2
  Stacks[i][STACKSIZE-7] = 0x01010101;   // R1
  Stacks[i][STACKSIZE-8] = 0x00000000;   // R0
  Stacks[i][STACKSIZE-9] = 0x11111111;   // R11
  Stacks[i][STACKSIZE-10] = 0x10101010;  // R10
  Stacks[i][STACKSIZE-11] = 0x09090909;  // R9
  Stacks[i][STACKSIZE-12] = 0x08080808;  // R8
  Stacks[i][STACKSIZE-13] = 0x07070707;  // R7
  Stacks[i][STACKSIZE-14] = 0x06060606;  // R6
  Stacks[i][STACKSIZE-15] = 0x05050505;  // R5
  Stacks[i][STACKSIZE-16] = 0x04040404;  // R4
}

// ******** OS_Init ************
// initialize operating system, disable interrupts until OS_Launch
// initialize OS controlled I/O: serial, ADC, systick, LaunchPad I/O and timers 
// input:  none
// output: none
// Timer1 used for OS system time
void OS_Init(void){
	uint32_t delay;
	OS_DisableInterrupts();
  PLL_Init();                 // set processor clock to 80 MHz
	SYSCTL_RCGCTIMER_R |= SYSCTL_RCGCTIMER_R1;   // activate timer1
	delay = SYSCTL_RCGCTIMER_R;   // allow time to finish activating
	TIMER1_CTL_R &= ~TIMER_CTL_TAEN; // disable TimerA1
	TIMER1_CFG_R  = TIMER_CFG_32_BIT_TIMER; // configure for 32-bit mode
	TIMER1_TAMR_R = TIMER_TAMR_TAMR_PERIOD;
	TIMER1_TAILR_R = (0x01<<31)-1;
	TIMER1_TAPR_R = 0; // set prescale = 0
	TIMER1_CTL_R |= TIMER_CTL_TAEN; // disable TimerA0
	#ifdef SYSTICK
  NVIC_ST_CTRL_R = 0;         // disable SysTick during setup
  NVIC_SYS_PRI3_R =(NVIC_SYS_PRI3_R & ~NVIC_SYS_PRI3_TICK_M)|(0x7 << NVIC_SYS_PRI3_TICK_S); // priority 7
	
	#endif
	NVIC_SYS_PRI3_R = (NVIC_SYS_PRI3_R&(~NVIC_SYS_PRI3_PENDSV_M))|(0x7 << NVIC_SYS_PRI3_PENDSV_S); // PendSV priority 7
	//NVIC_SYS_HND_CTRL_R |= NVIC_SYS_HND_CTRL_PNDSV; //enable PendSV
}

// ******** OS_InitSemaphore ************
// initialize semaphore 
// input:  pointer to a semaphore
// output: none
void OS_InitSemaphore(Sema4Type *semaPt, long value){
	int32_t status;
	status = StartCritical();
	semaPt->Value = value;
	EndCritical(status);
}

// DA 2/18
// ******** OS_Wait ************
// decrement semaphore 
// Lab2 spinlock
// Lab3 block if less than zero
// input:  pointer to a counting semaphore
// output: none
// From book pg. 191
void OS_Wait(Sema4Type *semaPt){
	
	OS_DisableInterrupts();
	while(semaPt->Value <= 0)
	{
		OS_EnableInterrupts();
		OS_Suspend();
		OS_DisableInterrupts();
	}
	semaPt->Value = semaPt->Value - 1;
	OS_EnableInterrupts();
}	

// DA 2/18
// ******** OS_Signal ************
// increment semaphore 
// Lab2 spinlock
// Lab3 wakeup blocked thread if appropriate 
// input:  pointer to a counting semaphore
// output: none
// From book pg. 191
void OS_Signal(Sema4Type *semaPt)
{	
	int32_t status;
	status = StartCritical();
	semaPt->Value = semaPt->Value + 1;
	EndCritical(status);
}	

// DA 2/18
// ******** OS_bWait ************
// Lab2 spinlock, set to 0
// Lab3 block if less than zero
// input:  pointer to a binary semaphore
// output: none
void OS_bWait(Sema4Type *semaPt){
	
	OS_DisableInterrupts();
	while(semaPt->Value == 0)
	{
		OS_EnableInterrupts();
		OS_Suspend();										//cooperative
		OS_DisableInterrupts();
	}
	// it exits while loop once 
	// semaphore has been signaled
	semaPt->Value = 0;
	OS_EnableInterrupts();
}

// DA 2/18
// ******** OS_bSignal ************
// Lab2 spinlock, set to 1
// Lab3 wakeup blocked thread if appropriate 
// input:  pointer to a binary semaphore
// output: none
void OS_bSignal(Sema4Type *semaPt)
{
	semaPt->Value = 1; // atomic
}

//******** OS_AddThread *************** 
// add a foregound thread to the scheduler
// Inputs: pointer to a void/void foreground task
//         number of bytes allocated for its stack
//         priority, 0 is highest, 5 is the lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// stack size must be divisable by 8 (aligned to double word boundary)
// In Lab 2, you can ignore both the stackSize and priority fields
// In Lab 3, you can ignore the stackSize fields
uint32_t g_NumAliveThreads=0;
int OS_AddThread(void(*task)(void), 
  unsigned long stackSize, unsigned long priority){ 
	uint32_t k=0, first, second;
//This implementation statically links threads depending on arbitrary order, and does not support dynamic adding and killing
//	long status = StartCritical();
//	if(i>NUMTHREADS){return 0;} //If max threads have been added return failure
//	tcbs[i].ID=i;
//	tcbs[i].Priority=priority;
//	tcbs[i].SleepCtr=0;
//	if(i>0){
//		 tcbs[i-1].next = &tcbs[i];  //Set previously added thread's next to the thread just added
//		 tcbs[i].previous=&tcbs[i-1]; //Create a back link from the thread we just left
//		 tcbs[i].next = &tcbs[0];			//Set the thread just added's next to the first thread in the linked list
//	}else{
//		tcbs[0].next = &tcbs[i];
//	}
//	tcbs[0].previous = &tcbs[i];
//  SetInitialStack(i); // initializes certain registers to arbitrary values
//	Stacks[i][stackSize-2] = (int32_t)(task); // PC
//	i++;
//  EndCritical(status);
	long status = StartCritical();
	if(g_NumAliveThreads>=NUMTHREADS){return 0;} //If max threads have been added return failure
	tcbs[g_NumAliveThreads].ID=g_NumAliveThreads;
  tcbs[g_NumAliveThreads].Priority=priority;
	tcbs[g_NumAliveThreads].SleepCtr=0;
	if(g_NumAliveThreads==0){
		RunPt=&tcbs[0];     //First thread added at start of OS
		tcbs[0].next=&tcbs[0];
		tcbs[0].previous = &tcbs[0];
		SetInitialStack(0); // initializes certain registers to arbitrary values
		Stacks[0][stackSize-2] = (int32_t)(task); // PC
		tcbs[0].MemStatus=USED;
		g_NumAliveThreads++;
	}

//	else if(g_NumAliveThreads==1){		//Second thread added to establish a circle
//		tcbs[0].previous = &tcbs[1];
//		tcbs[0].next = &tcbs[1];
//		tcbs[1].next = &tcbs[0];
//		tcbs[1].previous = &tcbs[0];
//		SetInitialStack(1); // initializes certain registers to arbitrary values
//		Stacks[1][stackSize-2] = (int32_t)(task); // PC
//		tcbs[1].MemStatus=USED;
//		g_NumAliveThreads++;
//	}
	else{
			for(k=0; k<NUMTHREADS; k++){
			if(tcbs[k].MemStatus==FREE){
				if(g_NumAliveThreads==1)
				{
					RunPt->previous=&tcbs[k];
					RunPt->next = &tcbs[k];
					tcbs[k].next=RunPt;
					tcbs[k].previous=RunPt;
					SetInitialStack(k);
					Stacks[k][stackSize-2] = (int32_t)(task); // PC
					g_NumAliveThreads++;
					tcbs[k].MemStatus=USED;
					break;
				}else{
					tcbs[k].next=RunPt->next;
					RunPt->next->previous = &tcbs[k];
					tcbs[k].previous=RunPt;
					RunPt->next=&tcbs[k];
					SetInitialStack(k); // initializes certain registers to arbitrary values
					Stacks[k][stackSize-2] = (int32_t)(task); // PC
					g_NumAliveThreads++;
					tcbs[k].MemStatus=USED;
					break;
				}
			}
		}
	}

	EndCritical(status);
  return 1;               // successful;
}

//******** OS_Id *************** 
// returns the thread ID for the currently running thread
// Inputs: none
// Outputs: Thread ID, number greater than zero 
unsigned long OS_Id(void){
	return RunPt->ID;
}


void OS_LaunchThread(void(*taskPtr)(void), int timer);
// initializes a new thread with given period and priority
int OS_AddPeriodicThread(void(*task)(void), int timer, unsigned long period, unsigned long priority)
{// period and priority are used when initializing the timer interrupts
	int status;
	int sr;
	sr = StartCritical();
	// initialize a timer specific to this thread
	// each timer should be unique to a thread so that it can interrupt when
	// it counts to 0 and sets the flag, this requires counting timers
	
	status = 0;
	status = TIMER_TimerInit(task,timer, period, priority);
	
	if(status == -1)
	{
		//printf("Error Initializing timer number(0-11): %d\n", timer);
	}
	OS_LaunchThread(task,timer);
	EndCritical(sr);
	return 0;
}

//******** OS_AddSwitchTasks *************** 
// add a background task to run whenever the SW1 (PF4) button is pushed
// Inputs: pointer to a void/void background function
//         priority 0 is the highest, 5 is the lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// It is assumed that the user task will run to completion and return
// This task can not spin, block, loop, sleep, or kill
// This task can call OS_Signal  OS_bSignal	 OS_AddThread
// This task does not have a Thread ID
// In labs 2 and 3, this command will be called 0 or 1 times
// In lab 2, the priority field can be ignored
// In lab 3, there will be up to four background threads, and this priority field 
//           determines the relative priority of these four threads
#define PF0       (*((volatile uint32_t *)0x40025004))
#define PF4       (*((volatile uint32_t *)0x40025040))
void (*PF4Task) (void);
void (*PF0Task) (void);
uint32_t static LastPF4, LastPF0;
int OS_AddSwitchTasks(void(*task1)(void), void(*task2)(void),unsigned long priority){
	long sr;
	sr = StartCritical();
	uint32_t delay;
	SYSCTL_RCGCGPIO_R |= 0x00000020;  // activate clock for Port F
  delay = SYSCTL_RCGCGPIO_R;        // allow time for clock to start
	PF4Task = task1;
	PF0Task = task2;
  GPIO_PORTF_LOCK_R = 0x4C4F434B;   // unlock GPIO Port F
  GPIO_PORTF_CR_R = 0x1F;           // allow changes to PF4-0
  // only PF0 needs to be unlocked, other bits can't be locked
  GPIO_PORTF_AMSEL_R &= ~0x11;        // isable analog on PF
  GPIO_PORTF_PCTL_R |= 0x11;   // PCTL GPIO on PF4,PF0
  GPIO_PORTF_DIR_R &= ~0x11;          // PF4,PF0 in
  GPIO_PORTF_AFSEL_R &= ~0x11;        // disable alt funct on PF4-0
  GPIO_PORTF_PUR_R |= 0x11;          // enable pull-up on PF0 and PF4
  GPIO_PORTF_DEN_R |= 0x11;          // enable digital I/O on PF4,PF0
	GPIO_PORTF_IS_R &= ~0x11;					//PF4,PF0 are edge sensitive
	GPIO_PORTF_IBE_R |= 0x11;					//Interrupt on both edges
	GPIO_PORTF_ICR_R |= 0x11;					//clear flags
	GPIO_PORTF_IM_R |= 0x11; 					//Arm interrupts
	LastPF4 = PF4;
	LastPF0 = PF0;
	NVIC_PRI7_R = (NVIC_PRI7_R&NVIC_PRI7_INT30_M)|(priority<<NVIC_PRI7_INT30_S);
	NVIC_EN0_R = NVIC_EN0_INT30;
	EndCritical(sr);
	return 1;
}

void static DebounceSW1Task(void){
	OS_Sleep(2);
	LastPF4 = PF4;									//Store current value of switch
	GPIO_PORTF_ICR_R |= 0x10;				//acknowledge interrupt
	GPIO_PORTF_IM_R |= 0x10;				//Re-arm interrupt
	OS_Kill();
}

void static DebounceSW2Task(void){
	OS_Sleep(2);
	LastPF0 = PF0;									//Store current value of switch
	GPIO_PORTF_ICR_R |= 0x01;				//acknowledge
	GPIO_PORTF_IM_R |= 0x01;				//Re-arm
	OS_Kill();
}
#define PE5  (*((volatile unsigned long *)0x40024080))
int interrupt_count = 0;
void GPIOPortF_Handler(void){
	uint32_t pin;
	interrupt_count++;
	pin = GPIO_PORTF_RIS_R&0x11;   //which switch triggered the interrupt?
	GPIO_PORTF_ICR_R |= pin;				//acknowledge

	if(pin==0x10){  //PF4 pressed
		if(LastPF4==0x10){				//If rising edge, execute user task
			(*PF4Task)();		
		}
		GPIO_PORTF_IM_R &= ~pin;	//disarm interrupt on PF4
		if(OS_AddThread(&DebounceSW1Task,128,1)==0){
			GPIO_PORTF_IM_R |= pin;
		}
	}
	if(pin==0x01){
		if(LastPF0==0x01){				//If rising edge, execute user task
			(*PF0Task)();
		}
		GPIO_PORTF_IM_R &= ~pin;	//disarm interrupt on PF0
		if(OS_AddThread(&DebounceSW2Task,128,1)==0){
			GPIO_PORTF_IM_R |= pin;
		}
	}
}
//Ignore for now
//******** OS_AddSW2Task *************** 
// add a background task to run whenever the SW2 (PF0) button is pushed
// Inputs: pointer to a void/void background function
//         priority 0 is highest, 5 is lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// It is assumed user task will run to completion and return
// This task can not spin block loop sleep or kill
// This task can call issue OS_Signal, it can call OS_AddThread
// This task does not have a Thread ID
// In lab 2, this function can be ignored
// In lab 3, this command will be called will be called 0 or 1 times
// In lab 3, there will be up to four background threads, and this priority field 
//           determines the relative priority of these four threads
int OS_AddSW2Task(void(*task)(void), unsigned long priority){
	;
}

// DA 2/20
// ******** OS_Sleep ************
// place this thread into a dormant state
// input:  -number of msec to sleep
// output: none
// You are free to select the time resolution for this function
// OS_Sleep(0) implements cooperative multitasking;

void OS_Sleep(unsigned long sleepTime){
	int32_t status;

	if(sleepTime > 0)
	{
		status = StartCritical();
		RunPt->SleepCtr = sleepTime; // atomic operation
		//g_sleepingThreads[g_numSleepingThreads++] = RunPt->ID; 
		// store an array containing the ID's of sleeping threads
		// this can be used to create a linked list of sleeping threads
		//RunPt->previous->next = RunPt->next; //Link the prior thread to the next thread
		//RunPt->next->previous = RunPt->previous;
		EndCritical(status);
		OS_Suspend();
	}
}

// DA 2/20
// ******** OS_Kill ************
// kill the currently running thread, release its TCB and stack
// input:  none
// output: none
void OS_Kill(void){
	
	int32_t status;
	status = StartCritical(); 
	if(RunPt->next!= RunPt) // keep one thread running at all times
	{		
	//Remove the currently running thread
		RunPt->previous->next = RunPt->next; 
		RunPt->next->previous = RunPt->previous;
		// remove tcb from linked list by 
		// moving the previous nodes nextPtr
		// to the current node's nextPtr
		
		RunPt->sp = NULL;
		RunPt->MemStatus = FREE;
		// release reference to stack pointer
		g_NumAliveThreads--;
		EndCritical(status);
		OS_Suspend();
	}
	EndCritical(status);
}

// ******** OS_Suspend ************
// suspend execution of currently running thread
// scheduler will choose another thread to execute
// Can be used to implement cooperative multitasking 
// Same function as OS_Sleep(0)
// input:  none
// output: none
void OS_Suspend(void){
	uint32_t sysreg;
	long sr = StartCritical();
	sysreg = NVIC_SYS_HND_CTRL_R;
	NVIC_INT_CTRL_R |= NVIC_INT_CTRL_PEND_SV; // does a contex switch 
	OS_ClearMsTime(); // reset SysTick period
	EndCritical(sr);
}
 
// ******** OS_Fifo_Init ************
// Initialize the Fifo to be empty
// Inputs: size
// Outputs: none 
// In Lab 2, you can ignore the size field
// In Lab 3, you should implement the user-defined fifo size
// In Lab 3, you can put whatever restrictions you want on size
//    e.g., 4 to 64 elements
//    e.g., must be a power of 2,4,8,16,32,64,128
void OS_Fifo_Init(unsigned long size)
{
	g_FIFOSIZE = size;
	// does this need to be protected???
	// will it only be called at the beginning
	g_Fifo = &Fifo[0];
	g_fifoPutPtr = &g_Fifo[0];
	g_fifoGetPtr = &g_Fifo[0];
	
	g_roomLeft.Value = g_FIFOSIZE;
	g_dataAvailable.Value = 0;
	g_fifoMutex.Value = 1; 
}

// ******** OS_Fifo_Put ************
// Enter one data sample into the Fifo
// Called from the background, so no waiting 
// Inputs:  data
// Outputs: true if data is properly saved,
//          false if data not saved, because it was full
// Since this is called by interrupt handlers 
//  this function can not disable or enable interrupts
int OS_Fifo_Put(unsigned long data)
{
	int32_t status;
	unsigned long* nextPutPtr;
	nextPutPtr = g_fifoPutPtr + 1;
	
//	OS_Wait(&g_roomLeft);
//	OS_bWait(&g_fifoMutex); 
	
	if(nextPutPtr == &g_Fifo[g_FIFOSIZE])
	{ //wrap
		nextPutPtr = &g_Fifo[0];
	}
	if(nextPutPtr == g_fifoGetPtr)
	{ // check for full FIFO
		// You DON'T want to call OS_Signal(&g_dataAvailable 
		// if you didn't add more data to the fifo 
		status = FIFO_FAIL;
		OS_bSignal(&g_fifoMutex); // release lock on g_fifoPutPtr
	}
	else
	{ // only add data and update pointer if room left
		
		status = FIFO_SUCCESS;
		*(g_fifoPutPtr) = data; // store data at current index
		g_fifoPutPtr = nextPutPtr; // update PutPtr
		OS_bSignal(&g_fifoMutex); // release lock on g_fifoPutPtr
		OS_Signal(&g_dataAvailable); // signal you added data to the fifo and its ready to be read
	}
	return status;

} 

// ******** OS_Fifo_Get ************
// Remove one data sample from the Fifo
// Called in foreground, will spin/block if empty
// Inputs:  none
// Outputs: data 
unsigned long OS_Fifo_Get(void)
{
	unsigned long data;
	
	OS_Wait(&g_dataAvailable); // make sure there is no underflow & there is an element in the fifo to get
	OS_bWait(&g_fifoMutex); // lock out the g_fifoGetPtr from all other threads
	data = *(g_fifoGetPtr++); // get the data, then move to next index in fifo
	
	if(g_fifoGetPtr == &g_Fifo[g_FIFOSIZE])
	{ // wrap
		g_fifoGetPtr = &g_Fifo[0];
	}
	
	OS_bSignal(&g_fifoMutex); // release lock on g_fifoGetPtr
	OS_Signal(&g_roomLeft);
	
	return data;
}

// ******** OS_Fifo_Size ************
// Check the status of the Fifo
// Inputs: none
// Outputs: returns the number of elements in the Fifo
//          greater than zero if a call to OS_Fifo_Get will return right away
//          zero or less than zero if the Fifo is empty 
//          zero or less than zero if a call to OS_Fifo_Get will spin or block
long OS_Fifo_Size(void)
{
	return g_roomLeft.Value;
}

// DA 2/20
// ******** OS_MailBox_Init ************
// Initialize communication channel
// Clear mailboxData and set flag to empty
// Inputs:  none
// Outputs: none
void OS_MailBox_Init(void)
{
	int32_t status;
	status = StartCritical();
	g_mailboxData = 0;
	g_mailboxFree.Value = MAILBOX_EMPTY; // valid data can be put into mailbox
	g_mailboxDataValid.Value = DATA_NOT_VALID; //valid data hasn't been put into mailbox yet
	EndCritical(status);
}

// DA 2/20
// ******** OS_MailBox_Send ************
// enter mail into the MailBox
// Inputs:  data to be sent
// Outputs: none
// This function will be called from a foreground thread
// It will spin/block if the MailBox contains data not yet received 
void OS_MailBox_Send(unsigned long data)
{	
	OS_bWait(&g_mailboxFree); // make sure the previous data was received before sending more
	g_mailboxData = data;
	OS_bSignal(&g_mailboxDataValid); // indicate that we just sent new data
}

// DA 2/20
// ******** OS_MailBox_Recv ************
// remove mail from the MailBox
// Inputs:  none
// Outputs: data received
// This function will be called from a foreground thread
// It will spin/block if the MailBox is empty 
unsigned long OS_MailBox_Recv(void)
{
	unsigned long data;
	
	OS_bWait(&g_mailboxDataValid); // make sure the data was sent & mailbox is full before reading
	data = g_mailboxData;
	OS_bSignal(&g_mailboxFree); // signal that the mailbox is empty and can accept new data
	
	return data;
}

// ******** OS_Time ************
// return the system time using SysTick
// Inputs:  none
// Outputs: time in 12.5ns units, 0 to 2^24-1
// The time resolution should be less than or equal to 1us, and the precision 32 bits
// It is ok to change the resolution and precision of this function as long as 
//   this function and OS_TimeDifference have the same resolution and precision 
unsigned long OS_Time(void)
{
	// assumes an 80 MHz Clock
	return TIMER1_TAR_R;
}

// DA 2/22
// ******** OS_TimeDifference ************
// Calculates difference between two times
// Inputs:  two times measured with OS_Time
// Outputs: time difference in 12.5ns units 
// The time resolution should be less than or equal to 1us, and the precision at least 12 bits
// It is ok to change the resolution and precision of this function as long as 
// this function and OS_Time have the same resolution and precision 
unsigned long OS_TimeDifference(unsigned long start, unsigned long stop)
{
	// NOTE: This cannot accurately account for time differences
	// when you roll over. Requires: start > end & that both have been
	// read within the same SysTick Period
	unsigned long diff;
	diff = (start - stop);
	return diff;
}

// DA 2/22
// ******** OS_ClearMsTime ************
// sets the system time to zero (from Lab 1)
// Essentially disables SysTick
// Inputs:  none
// Outputs: none
// You are free to change how this works
void OS_ClearMsTime(void)
{
	//int32_t status = StartCritical();
	// Need to do this for SysTick since that
	// is what we are using for the system time
	// any write to NVIC_ST_CURRENT_R resets it to the
	// NVIC_ST_RELOAD_R value
	//NVIC_ST_CURRENT_R = 10;
	//EndCritical(status);
	
	g_msTime = 0; // atomic
	
}

// ******** OS_MsTime ************
// reads the current time in msec (from Lab 1)
// Inputs:  none
// Outputs: time in ms units
// You are free to select the time resolution for this function
// It is ok to make the resolution to match the first call to OS_AddPeriodicThread
unsigned long OS_MsTime(void)
{ // this is the msTime since SysTick has started counting
	// It only updates when SysTick interrupts occur
	return g_msTime; // it actually is in ms
}

//******** OS_Launch *************** 
// start the scheduler, enable interrupts
// Inputs: number of 12.5ns clock cycles for each time slice
//         you may select the units of this parameter
// Outputs: none (does not return)
// In Lab 2, you can ignore the theTimeSlice field
// In Lab 3, you should implement the user-defined TimeSlice field
// It is ok to limit the range of theTimeSlice to match the 24-bit SysTick
void OS_Launch(unsigned long theTimeSlice){
	//RunPt = &tcbs[0];       // thread 0 will run first    This is done in OS_AddThread
	#ifdef SYSTICK
	NVIC_ST_CURRENT_R = 0;      // any write to current clears it
	NVIC_ST_RELOAD_R = theTimeSlice - 1; // reload value
  NVIC_ST_CTRL_R = NVIC_ST_CTRL_ENABLE+NVIC_ST_CTRL_CLK_SRC+NVIC_ST_CTRL_INTEN;// enable, core clock and interrupt arm
	#endif
  StartOS();                   // start on the first task
}


// Resets the 32-bit counter to zero
// DA 2/20	
// SERIOUS POTENTIAL PROBLEM, IF WE
// ARE LOOKING AT TIME DIFFERENCES AND
// WE CLEAR THIS OUT BUT DON'T STOP
// A TASK FROM LOOKING AT THE TIME DIFFERENCE
// IT WILL USE AN INCORRECT TIME DIFF
// SINCE WE FORCED IT INTO RESET
void OS_ClearPeriodicTime(int timer)
{
	TIMER_ClearPeriodicTime(timer);
}

// Returns the number of bus cycles in a full period
unsigned long OS_ReadTimerPeriod(int timer)
{
	return TIMER_ReadTimerPeriod(timer);
}

unsigned long OS_ReadTimerValue(int timer)
{
	return TIMER_ReadTimerValue(timer); 
}

// enables Timer interrupts in the NVIC vector table
void OS_NVIC_EnableTimerInt(int timer)
{
	TIMER_NVIC_EnableTimerInt(timer);
}

// disables Timer interrupts in the NVIC vector table
void OS_NVIC_DisableTimerInt(int timer)
{
	TIMER_NVIC_DisableTimerInt(timer);
}

// begin the timers and start the task in the interrupt
// the function ptr isn't necessary here for the periodic threads
void OS_LaunchThread(void(*taskPtr)(void), int timer)
{
	int timerN;
	unsigned int TAEN_TBEN;

	timerN = timer; // its the timerID (0 to 11)
	OS_NVIC_EnableTimerInt(timer);
	TAEN_TBEN = ((timerN%2) ? TIMER_CTL_TBEN : TIMER_CTL_TAEN); // if timerN even => TAEN, timerN odd => TBEN
	*(timerCtrlBuf[timerN]) |= TAEN_TBEN; // start timer for this thread, X in {0,1,2,3,4,5}, x in {A,B}
	// ^^^ is *(TIMERX_CTRL_PTR_R) |= TIMER_CTL_TxEN and/or TIMERX_CTL_R |= TIMER_CTL_TxEN
	//(*taskPtr)(); // begin task for this thread Do this in the ISR
}

// disable periodic timer interrupt 
void OS_StopThread(void(*taskPtr)(void), int timer)
{
	int timerN;
	unsigned int TAEN_TBEN;

	timerN = timer; // its the timerID (0 to 11)
	OS_NVIC_DisableTimerInt(timer);
	TAEN_TBEN = ((timerN%2) ? TIMER_CTL_TBEN : TIMER_CTL_TAEN); // if timerN even => TAEN, timerN odd => TBEN
	*(timerCtrlBuf[timerN]) &= ~TAEN_TBEN; // start timer for this thread, X in {0,1,2,3,4,5}, x in {A,B}
	// ^^^ is *(TIMERX_CTRL_PTR_R) |= TIMER_CTL_TxEN and/or TIMERX_CTL_R |= TIMER_CTL_TxEN
	//(*taskPtr)(); // begin task for this thread Do this in the ISR
}

// this configures the timers for 32-bit mode, periodic mode
int OS_TimerInit(void(*task)(void), int timer, unsigned long desiredFrequency, unsigned long priority)
{
	TIMER_TimerInit(task,timer,desiredFrequency,priority);
	return 0;
}


//Debugging Port F
void GPIO_PortF_Init(void)
{
	unsigned long delay;
	SYSCTL_RCGCGPIO_R |= SYSCTL_RCGC2_GPIOF;
	delay = SYSCTL_RCGCGPIO_R;
	
	GPIO_PORTF_DIR_R |= 0x0F; // PF0-3 output
	GPIO_PORTF_DEN_R |= 0x0F; // enable Digital IO on PF0-3
	GPIO_PORTF_AFSEL_R &= ~0x0F; // PF0-3 alt funct disable
	GPIO_PORTF_AMSEL_R &= ~0x0F; // disable analog functionality on PF0-3
	
	GPIO_PORTF_DATA_R = 0x06;
}


void PF0_Toggle(void)
{	
	GPIO_PORTF_DATA_R ^= 0x01;
}

void PF1_Toggle(void)
{
		GPIO_PORTF_DATA_R ^= 0x02;
}
void PF2_Toggle(void)
{	
	GPIO_PORTF_DATA_R ^= 0x04;
}

void PF3_Toggle(void)
{
		GPIO_PORTF_DATA_R ^= 0x08;
}

void Jitter(void){;}

//__asm  

void SysTick_Handler(void)
{
	int status;
	tcbType* sleepIterator;
	status = StartCritical();
#define SYSTICK_PERIOD 1 //Systick interrupts every 1 ms so decrement sleep counters by 1 
	g_msTime += SYSTICK_PERIOD;
	
	if(RunPt->SleepCtr > 0)			//if the sleep counter of the running thread is nonzero, decrement it
	{
		RunPt->SleepCtr -= SYSTICK_PERIOD;
	}
	for(sleepIterator = RunPt->next; sleepIterator != RunPt; sleepIterator = sleepIterator->next)
	{
		//Iterate through the linked list until all sleeping threads have been decremented
		if(sleepIterator->SleepCtr > 0)
		{
			sleepIterator->SleepCtr -= SYSTICK_PERIOD;
		}	
	}

	EndCritical(status);
	PE5^=0xFF;
	OS_Suspend(); //context switch
	PE5^=0xFF;
}
	
	
