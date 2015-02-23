
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

#define MAILBOX_EMPTY	0
#define MAILBOX_FULL	1

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
int32_t g_sleepingThreads[NUMTHREADS];
int32_t g_numSleepingThreads = 0;

volatile int mutex;
volatile int RoomLeft;
volatile int CurrentSize;
Sema4Type LCDmutex;
unsigned long g_mailboxData;
int32_t g_mailboxFlag;
unsigned long* g_ulFifo; // pointer to OS_FIFO

unsigned long g_getIndex; // get ptr for OS_FIFO
unsigned long g_putIndex; // put ptr for OS_FIFO

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
		OS_DisableInterrupts();
	}
	semaPt->Value = 0;
	OS_EnableInterrupts();
}

// DA 2/18
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
		 tcbs[i].previous=&tcbs[i-1]; //Create a back link from the thread we just left
		 tcbs[i].next = &tcbs[0];			//Set the thread just added's next to the first thread in the linked list
	}else{
		tcbs[0].next = &tcbs[i];
	}
	tcbs[0].previous = &tcbs[i];
  SetInitialStack(i); // initializes certain registers to arbitrary values
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
	return RunPt->ID;
}



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

// DA 2/20
// ******** OS_Sleep ************
// place this thread into a dormant state
// input:  -number of msec to sleep
// output: none
// You are free to select the time resolution for this function
// OS_Sleep(0) implements cooperative multitasking;
void OS_Sleep(unsigned long sleepTime){
	int32_t status = StartCritical();
	RunPt->SleepCtr = sleepTime; // atomic operation
	g_sleepingThreads[g_numSleepingThreads++] = RunPt->ID; 
	// store an array containing the ID's of sleeping threads
	// this can be used to create a linked list of sleeping threads
	RunPt->previous->next = RunPt->next; //Link the prior thread to the next thread
	RunPt->next->previous = RunPt->previous;
	EndCritical(status);
	OS_Suspend();
}

// DA 2/20
// ******** OS_Kill ************
// kill the currently running thread, release its TCB and stack
// input:  none
// output: none
void OS_Kill(void){
	
	int32_t status;
	status = StartCritical();
	RunPt->previous->next = RunPt->next; 
	RunPt->next->previous = RunPt->previous;
	// remove tcb from linked list by 
	// moving the previous nodes nextPtr
	// to the current node's nextPtr
	
	RunPt->sp = NULL;
	// release reference to stack pointer
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
void OS_Fifo_Init(unsigned long size){

	// does this need to be protected???
	// will it only be called at the beginning
	g_ulFifo = (unsigned long*) malloc(sizeof(unsigned long) * size);
	g_putIndex = 0;
	g_getIndex = 0;
}

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

// DA 2/20
// ******** OS_MailBox_Init ************
// Initialize communication channel
// Clear mailboxData and set flag to empty
// Inputs:  none
// Outputs: none
void OS_MailBox_Init(void){

	int32_t status;
	status = StartCritical();
	g_mailboxData = 0;
	g_mailboxFlag = MAILBOX_EMPTY;
	EndCritical(status);
}

// DA 2/20
// ******** OS_MailBox_Send ************
// enter mail into the MailBox
// Inputs:  data to be sent
// Outputs: none
// This function will be called from a foreground thread
// It will spin/block if the MailBox contains data not yet received 
void OS_MailBox_Send(unsigned long data){
	// don't have to worry about critical sections when writing/reading
	// data bc the Flag synchronizes/locks the shared resource
	g_mailboxData = data;
	g_mailboxFlag = MAILBOX_FULL;
}

// DA 2/20
// ******** OS_MailBox_Recv ************
// remove mail from the MailBox
// Inputs:  none
// Outputs: data received
// This function will be called from a foreground thread
// It will spin/block if the MailBox is empty 
unsigned long OS_MailBox_Recv(void){

	g_mailboxFlag = MAILBOX_EMPTY;
	// I am worried about a critical section if it interrupts
	// here and it signifies the mbox empty when it has data that
	// hasn't been read and hasn't yet returned. It depends on how
	// it is used which determines if it is critical or not since you can't 
	// release the resource after you've returned
	return g_mailboxData;
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
	return NVIC_ST_CURRENT_R;
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
	diff = start - stop;
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
	int32_t status = StartCritical();
	// Need to do this for SysTick since that
	// is what we are using for the system time
	NVIC_ST_RELOAD_R = 0;
	NVIC_ST_CURRENT_R = 0;
	EndCritical(status);
}

// ******** OS_MsTime ************
// reads the current time in msec (from Lab 1)
// Inputs:  none
// Outputs: time in ms units
// You are free to select the time resolution for this function
// It is ok to make the resolution to match the first call to OS_AddPeriodicThread
unsigned long OS_MsTime(void)
{
	
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
	RunPt = &tcbs[0];       // thread 0 will run first
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
	// first decrement any sleeping threads
	// assumed to interrupt on 2ms periodic intervals
	int i;
	int threadsAwakened;
	int status;
	int awakenedThreads;
	status = StartCritical();
	
	threadsAwakened = 0;

#define SYSTICK_PERIOD 2		
	for(i = 0; i < g_numSleepingThreads; i++)
	{
		// I am not sure whether we should hardcode the timeDifference
		// or use a function for it. the former is faster, the latter is 
		// more stable. If we change SysTick frequency we need to remember 
		// to change the hardcoded value too. or we could just use a variable 
		// and set it where we initialize systick. I will hardcode a macro for now
		tcbs[g_sleepingThreads[i]].SleepCtr -= SYSTICK_PERIOD;
		
		// come out of sleep state to the active thread state and put 
		// it back in round robin for the scheduler, where should it go???
		// Is there a preferrable way to do this? Run it first???
		//KL 2/21
		//We can leave sleeping threads in the linked list if we want and just execute
		//this everytime a Systick is called. This isn't the most efficient but I remember it was mentioned in class
		//for(0:number of threads){
		//	if(sleep_counter>=0)
		//    decrement sleep_counter
		// }
		// or schedulue based on the value of sleepCtr. e.g. if period is 5 ms
		// and we have 4 threads sleeping with sleepCtr = {4,3,2,1}
		// should we run the lowest sleepCtr value first???
		if(tcbs[g_sleepingThreads[i]].SleepCtr <= 0)
		{
			threadsAwakened++;
			// Add to scheduler
			// we also need to make sure there isn't an interrupt driven
			// periodic thread that is trying to access this value, need a mutex
			// problem is we would need one for each variable in each tcb not just 
			// one mutex for all of them, bc others could change if they aren't accessing
			// this particular sleepCtr. Or should we just StartCritical-EndCritical???
			//OS_bWait(&semaphoreSleep0) // need more, not sure how to do this
			// tcbs[g_sleepingThreads[i]].SleepCtr = 0;
			//OS_bSignal(&semaphoreSleep0);
		
			tcbs[g_sleepingThreads[i]].next = RunPt->next; // insert thread between current thread and next thread
			tcbs[g_sleepingThreads[i]].previous = RunPt; // give the added thread a next and previous tcb
			
			// update other tcbs in the Linked list
			RunPt->next->previous = &tcbs[g_sleepingThreads[i]];
			RunPt->next = &tcbs[g_sleepingThreads[i]];
			awakenedThreads++;
			//tcbs[g_sleepingThreads[i]].next->previous->next = &tcbs[g_sleepingThreads[i]]; // insert thread between current thread and next thread
			//tcbs[g_sleepingThreads[i]].previous->next->previous = &tcbs[g_sleepingThreads[i]]; // give the added thread a next and previous tcb
		}			
	}

	g_numSleepingThreads -= threadsAwakened; // update number of sleeping threads

	
	// next do the context switch - currently round robin
	//PendSV_Handler();
	EndCritical(status);
	OS_Suspend();
}
	
	
