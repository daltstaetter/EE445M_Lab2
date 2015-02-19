
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
//#define SYSTICK // defined in ifdef.h

// function definitions in osasm.s
void OS_DisableInterrupts(void); // Disable interrupts
void OS_EnableInterrupts(void);  // Enable interrupts
int32_t StartCritical(void);
void EndCritical(int32_t primask);
void StartOS(void);

#define NUMTHREADS 3
#define STACKSIZE 128
struct tcb{
	int32_t *sp;
	struct tcb *next;
	struct tcb *previous;
	int32_t ID;
	int32_t SleepCtr;
	int32_t Priority;
};
typedef struct tcb tcbType;
tcbType tcbs[NUMTHREADS];
tcbType *RunPt;
int32_t Stacks[NUMTHREADS][STACKSIZE];

volatile int mutex;
volatile int RoomLeft;
volatile int CurrentSize;

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
void OS_Init(void){
	OS_DisableInterrupts();
  PLL_Init();                 // set processor clock to 80 MHz
	#ifdef SYSTICK
  NVIC_ST_CTRL_R = 0;         // disable SysTick during setup
  NVIC_SYS_PRI3_R =(NVIC_SYS_PRI3_R&0x00FFFFFF)|0xE0000000; // priority 7
	#endif
	NVIC_SYS_PRI3_R = (NVIC_SYS_PRI3_R&(~NVIC_SYS_PRI3_PENDSV_M))|(0x7 << NVIC_SYS_PRI3_PENDSV_S); // PendSV priority 7
	//NVIC_SYS_HND_CTRL_R |= NVIC_SYS_HND_CTRL_PNDSV; //enable PendSV
}

// ******** OS_InitSemaphore ************
// initialize semaphore 
// input:  pointer to a semaphore
// output: none
void OS_InitSemaphore(Sema4Type *semaPt, long value){
	;
}


// ******** OS_Wait ************
// decrement semaphore 
// Lab2 spinlock
// Lab3 block if less than zero
// input:  pointer to a counting semaphore
// output: none
void OS_Wait(Sema4Type *semaPt){
	
	OS_DisableInterrupts();
	while(semaPt->Value < 0)
	{
		OS_EnableInterrupts();
		OS_DisableInterrupts();
	}
	semaPt->Value = semaPt->Value - 1;
	OS_EnableInterrupts();
}	

// ******** OS_Signal ************
// increment semaphore 
// Lab2 spinlock
// Lab3 wakeup blocked thread if appropriate 
// input:  pointer to a counting semaphore
// output: none
void OS_Signal(Sema4Type *semaPt)
{	
	int32_t status;
	status = StartCritical();
	semaPt->Value = semaPt->Value + 1;
	EndCritical(status);
}	

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
		OS_DisableInterrupts();
	}
	semaPt->Value = 0;
	OS_EnableInterrupts();
}

// ******** OS_bSignal ************
// Lab2 spinlock, set to 1
// Lab3 wakeup blocked thread if appropriate 
// input:  pointer to a binary semaphore
// output: none
void OS_bSignal(Sema4Type *semaPt){
	
	int32_t status;
	
	status = StartCritical();
	semaPt->Value = 1;
	EndCritical(status);
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
int OS_AddThread(void(*task)(void), 
  unsigned long stackSize, unsigned long priority){
	static uint32_t i=0; 
	long status = StartCritical();
	if(i>NUMTHREADS){return 0;} //If max threads have been added return failure
	tcbs[i].ID=i;
	tcbs[i].Priority=priority;
	tcbs[i].SleepCtr=0;
	if(i>0){
		 tcbs[i-1].next = &tcbs[i];  //Set previously added thread's next to the thread just added
		 tcbs[i].previous=&tcbs[i-1];
		 tcbs[i].next = &tcbs[0];			//Set the thread just added's next to the first thread in the linked list
	}else{
		tcbs[0].next=&tcbs[0];
		tcbs[0].previous=&tcbs[0];
	}
  SetInitialStack(i); 
	Stacks[i][stackSize-2] = (int32_t)(task); // PC
	i++;
  EndCritical(status);
  return 1;               // successful;
}

//******** OS_Id *************** 
// returns the thread ID for the currently running thread
// Inputs: none
// Outputs: Thread ID, number greater than zero 
unsigned long OS_Id(void){
	long sr = StartCritical();
	return RunPt->ID;
	EndCritical(sr);
}


extern void(*HandlerTaskArray[12])(void); // Holds the function pointers to the threads that will be launched

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
	EndCritical(sr);
	return 0;
}

//******** OS_AddSW1Task *************** 
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
int OS_AddSW1Task(void(*task)(void), unsigned long priority){
	;
}

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

// ******** OS_Sleep ************
// place this thread into a dormant state
// input:  number of msec to sleep
// output: none
// You are free to select the time resolution for this function
// OS_Sleep(0) implements cooperative multitasking
void OS_Sleep(unsigned long sleepTime){
	;
}

// ******** OS_Kill ************
// kill the currently running thread, release its TCB and stack
// input:  none
// output: none
void OS_Kill(void){
	;
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
	sysreg= NVIC_SYS_HND_CTRL_R;
	NVIC_INT_CTRL_R |= NVIC_INT_CTRL_PEND_SV;
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
void OS_Fifo_Init(unsigned long size){;}

// ******** OS_Fifo_Put ************
// Enter one data sample into the Fifo
// Called from the background, so no waiting 
// Inputs:  data
// Outputs: true if data is properly saved,
//          false if data not saved, because it was full
// Since this is called by interrupt handlers 
//  this function can not disable or enable interrupts
int OS_Fifo_Put(unsigned long data){;} 

// ******** OS_Fifo_Get ************
// Remove one data sample from the Fifo
// Called in foreground, will spin/block if empty
// Inputs:  none
// Outputs: data 
unsigned long OS_Fifo_Get(void){;}

// ******** OS_Fifo_Size ************
// Check the status of the Fifo
// Inputs: none
// Outputs: returns the number of elements in the Fifo
//          greater than zero if a call to OS_Fifo_Get will return right away
//          zero or less than zero if the Fifo is empty 
//          zero or less than zero if a call to OS_Fifo_Get will spin or block
long OS_Fifo_Size(void){;}

// ******** OS_MailBox_Init ************
// Initialize communication channel
// Inputs:  none
// Outputs: none
void OS_MailBox_Init(void){;}

// ******** OS_MailBox_Send ************
// enter mail into the MailBox
// Inputs:  data to be sent
// Outputs: none
// This function will be called from a foreground thread
// It will spin/block if the MailBox contains data not yet received 
void OS_MailBox_Send(unsigned long data){;}

// ******** OS_MailBox_Recv ************
// remove mail from the MailBox
// Inputs:  none
// Outputs: data received
// This function will be called from a foreground thread
// It will spin/block if the MailBox is empty 
unsigned long OS_MailBox_Recv(void){;}

// ******** OS_Time ************
// return the system time 
// Inputs:  none
// Outputs: time in 12.5ns units, 0 to 4294967295
// The time resolution should be less than or equal to 1us, and the precision 32 bits
// It is ok to change the resolution and precision of this function as long as 
//   this function and OS_TimeDifference have the same resolution and precision 
unsigned long OS_Time(void){;}

// ******** OS_TimeDifference ************
// Calculates difference between two times
// Inputs:  two times measured with OS_Time
// Outputs: time difference in 12.5ns units 
// The time resolution should be less than or equal to 1us, and the precision at least 12 bits
// It is ok to change the resolution and precision of this function as long as 
//   this function and OS_Time have the same resolution and precision 
unsigned long OS_TimeDifference(unsigned long start, unsigned long stop){;}

// ******** OS_ClearMsTime ************
// sets the system time to zero (from Lab 1)
// Inputs:  none
// Outputs: none
// You are free to change how this works
void OS_ClearMsTime(void){;}

// ******** OS_MsTime ************
// reads the current time in msec (from Lab 1)
// Inputs:  none
// Outputs: time in ms units
// You are free to select the time resolution for this function
// It is ok to make the resolution to match the first call to OS_AddPeriodicThread
unsigned long OS_MsTime(void){;}

//******** OS_Launch *************** 
// start the scheduler, enable interrupts
// Inputs: number of 12.5ns clock cycles for each time slice
//         you may select the units of this parameter
// Outputs: none (does not return)
// In Lab 2, you can ignore the theTimeSlice field
// In Lab 3, you should implement the user-defined TimeSlice field
// It is ok to limit the range of theTimeSlice to match the 24-bit SysTick
void OS_Launch(unsigned long theTimeSlice){
	RunPt = &tcbs[0];       // thread 0 will run first
	#ifdef SYSTICK
	NVIC_ST_CURRENT_R = 0;      // any write to current clears it
	NVIC_ST_RELOAD_R = theTimeSlice - 1; // reload value
  NVIC_ST_CTRL_R = NVIC_ST_CTRL_ENABLE+NVIC_ST_CTRL_CLK_SRC+NVIC_ST_CTRL_INTEN;// enable, core clock and interrupt arm
	#endif
  StartOS();                   // start on the first task
}


// Resets the 32-bit counter to zero
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


// launches all programs
//void OS_LaunchAll(void(**taskPtrPtr)(void))
//{
////	int i;
////	int timerN;
////	unsigned int TAEN_TBEN;
////	for(i = 0; i <= timerCount; i++)
////	{
////		timerN = usedTimers[i]; // its the timerID 0 to 11
////		TAEN_TBEN = ((timerN%2) ? TIMER_CTL_TBEN:TIMER_CTL_TAEN); // if timerN even => TAEN, timerN odd => TBEN
////		*(timerCtrlBuf[timerN]) |= TAEN_TBEN;
////	}
//	
//	// this assumes that the timers initialized in the same sequence as the tasks
//	// i.e. usedTimer[i] was initialized for the functionPtr *(taskPtrPtr[i]) 
//	int i;
//	for(i = 0; i <= timerCount; i++)
//	{
//		OS_LaunchThread(taskPtrPtr[i],usedTimers[i]);
//	}
//}

// enables interrupts in the NVIC vector table
void OS_NVIC_EnableTimerInt(int timer)
{
	TIMER_NVIC_EnableTimerInt(timer);
}


void OS_NVIC_DisableTimerInt(int timer)
{
	TIMER_NVIC_DisableTimerInt(timer);
}


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
//
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

