#include <stdio.h>
#include <stdlib.h>
#include <sys/neutrino.h>
// include iofunc.h before dispatch.h or "ocb->" cant point
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/netmgr.h>

#define ATTACH_POINT "/dev/local/metronome"
#define METRONOME_PULSE_CODE _PULSE_CODE_MINAVAIL
// page 595 of Real-Time Embedded Systems: Design Principles and Engineering Practices
// _PULSE_CODE_MINAVAIL to MAX is safe
#define PAUSE_PULSE_CODE _PULSE_CODE_MINAVAIL+1
#define QUIT_PULSE_CODE _PULSE_CODE_MINAVAIL+2
#define DTR_row_length 8


typedef union {
	struct _pulse pulse;
} my_message_t;

struct DataTableRow {
	int time_sig_top;
	int time_sig_bottom;
	int interval_per_beat;
	char *interval;
};

// array
struct DataTableRow t[] = {
		{2, 4, 4, "|1&2&"},
		{3, 4, 6, "|1&2&3&"},
		{4, 4, 8, "|1&2&3&4&"},
		{5, 4, 10, "|1&2&3&4-5-"},
		{3, 8, 6, "|1-2-3-"},
		{6, 8, 6, "|1&a2&a"},
		{9, 8, 9, "|1&a2&a3&a"},
		{12, 8, 12, "|1&a2&a3&a4&a"}
};

// seconds per line
float time_per_measure;
// seconds per interval
float time_per_spacing;
int time_BPM = 0;
int time_pause = 0;
int counter = -1;
// beat per second
double time_BPS = 0;
unsigned long time_nano = 0;


char data[255];
int metronome_coid;
// from dispatch.h
name_attach_t *attach;

int thread_ID;

void *metronome_thread();
int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra);
int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb);
int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb);

void *metronome_thread() {
	// create thread in main; thread runs this function
	// purpose: "drive" metronome
	//			    receives pulse from interval timer; each time the timer expires
	//          receives pulses from io_write (quit and pause <int>)


	my_message_t msg;
	// metronome timer
	struct sigevent event;
	struct itimerspec itime;
	timer_t timer_id;
	// store receive ID of pulse
	int rcvid;
	int time_total_seconds = 0;
	int timer_counter;
	int interval_number;


	/*
  // Phase I - create a named channel to receive pulses
  call attach = name_attach( NULL, "metronome", 0 )

  calculate the seconds-per-beat and nano seconds for the interval timer
  create an interval timer to "drive" the metronome
  	configure the interval timer to send a pulse to channel at attach when it expires
	 */

	/* Create a local name (/dev/name/local/...) */
	attach = name_attach(NULL, "metronome", 0);

	if(attach == NULL) {
		printf("name_attach failed.\n");
		exit(EXIT_FAILURE);
	}

	time_BPS = (60 /(double) time_BPM) * (double)t[counter].time_sig_top / (double)t[counter].interval_per_beat;
	time_total_seconds = (int)(60 * (double)t[counter].time_sig_top / (double) time_BPM);
	// 1000000000 nano in 1 sec
	time_nano = time_BPS * 1000000000;

	// QNX timer http://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_lib_ref/t/timer_create.html
	event.sigev_notify   = SIGEV_PULSE;
	event.sigev_coid     = ConnectAttach(ND_LOCAL_NODE, 0, attach->chid,_NTO_SIDE_CHANNEL, 0);
	event.sigev_priority = SchedGet(0,0,NULL);
	event.sigev_code     = METRONOME_PULSE_CODE;
	event.sigev_value.sival_int = 0;

	itime.it_value.tv_sec = time_total_seconds;
	itime.it_value.tv_nsec = time_nano;
	itime.it_interval.tv_sec = time_BPS;
	itime.it_interval.tv_nsec = time_nano;

	// Phase II - receive pulses from interval timer OR io_write(pause, quit)
	for(;;) {
		timer_create(CLOCK_REALTIME, &event, &timer_id);
		timer_settime(timer_id, 0, &itime, NULL);
		interval_number = t[counter].interval_per_beat;
		timer_counter = 0;

		for (;;) {

			rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);

			if (rcvid == 0) {
				switch(msg.pulse.code) {
				case METRONOME_PULSE_CODE:
					//display the beat to stdout
					//  must handle 3 cases:
					//    start-of-measure: |1
					//    mid-measure: the symbol, as seen in the column "Pattern for Intervals within Each Beat"
					//    end-of-measure: \n
					fflush(stdout);
					if(timer_counter == 0) {
						timer_counter = 1;
						printf("\n%c%c", t[counter].interval[0], t[counter].interval[1]);
					} else {
						printf("%c", t[counter].interval[timer_counter]);
					}

					++timer_counter;

					if(timer_counter > interval_number) {
						timer_counter = 0;
					}
					continue;

				case PAUSE_PULSE_CODE:

					// pause the running timer for pause <int> seconds
					// AVOID: calling sleep()
					itime.it_value.tv_sec = time_pause;
					itime.it_value.tv_nsec = 0;
					timer_settime(timer_id, 0, &itime, NULL);
					continue;

				case QUIT_PULSE_CODE:
					// implement Phase III:
					//  delete interval timer
					//  call name_detach()
					//  call name_close()
					//  exit with SUCCESS
					timer_delete(timer_id);
					name_detach(attach, 0);
					name_close(metronome_coid);
					printf("\nQuitting.");
					exit(EXIT_SUCCESS);
				}
			}

		}
	}
	return EXIT_SUCCESS;
}

int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {

	/*
  //TODO: calculations for secs-per-beat, nanoSecs
  sprintf(data, "[metronome: %d beats/min, time signature %d/%d, secs-per-beat: %.2f, nanoSecs: %d]\n",

  nb = strlen(data);
	 */

	int nb;

	if(data == NULL)
		return 0;

	nb = strlen(data);

	//test to see if we have already sent the whole message.
	if (ocb->offset == nb)
		return 0;

	//We will return which ever is smaller the size of our data or the size of the buffer
	nb = min(nb, msg->i.nbytes);

	//Set the number of bytes we will return
	_IO_SET_READ_NBYTES(ctp, nb);

	//Copy data into reply buffer.
	SETIOV(ctp->iov, data, nb);

	//update offset into our data used to determine start position for next read.
	ocb->offset += nb;

	//If we are going to send any bytes update the access time for this resource.
	if (nb > 0)
		ocb->attr->flags |= IOFUNC_ATTR_ATIME;

	return(_RESMGR_NPARTS(1));
}

int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
	/*
  if (buf == "pause")     // similar to alert <int>
    process consoleValue
    validate consoleValue for range check
    MsgSendPulse(metronome_coid, priority, PAUSE_PULSE_CODE, consoleValue);

  if (buf == "quit")
    MsgSendPulse(metronome_coid, priority, QUIT_PULSE_CODE, 0;) 
	 */

	int nb = 0;

	if( msg->i.nbytes == ctp->info.msglen - (ctp->offset + sizeof(*msg) ))
	{
		/* have all the data */
		char *buf;
		char *console_msg;
		int i;
		int consoleValue = 0;
		buf = (char *)(msg+1);

		// from string.h
		// find the first occurrence of "pause" in buf
		if(strstr(buf, "pause") != NULL){
			for(i = 0; i < 2; i++){
				// slice string at " " for the string in &buf
				console_msg = strsep(&buf, " ");

			}

			consoleValue = atoi(console_msg);
			if(consoleValue >= 1 && consoleValue <= 9){
				//replace getprio() with SchedGet()
				MsgSendPulse(metronome_coid, SchedGet(0,0,NULL), PAUSE_PULSE_CODE, consoleValue);
				time_pause = consoleValue;
			} else {
				printf("\nInteger is not between 1 and 9 inclusive.\n");
			}
		} else if(strstr(buf, "info") != NULL) {
			sprintf(data, "[metronome: %d beats/min, time signature %d/%d, secs-per-beat: %.2f, nanoSecs: %d]\n", time_BPM, t[counter].time_sig_top, t[counter].time_sig_bottom, time_BPS, time_nano);
		} else if(strstr(buf, "quit") != NULL){
			MsgSendPulse(metronome_coid, SchedGet(0,0,NULL), QUIT_PULSE_CODE, 0);
		} else {
			printf("Invalid command.\n");
		}

		nb = msg->i.nbytes;
	}
	_IO_SET_WRITE_NBYTES (ctp, nb);

	if (msg->i.nbytes > 0)
		ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
	return (_RESMGR_NPARTS (0));

}

int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra) {
	/*
    	metronome_coid = name_open( "metronome", 0 );
    	return iofunc_default_open(...);
	 */
	if ((metronome_coid = name_open("metronome", 0)) == -1) {
		perror("name_open failed.");
		return EXIT_FAILURE;
	}
	return (iofunc_open_default (ctp, msg, handle, extra));
}

int main (int argc, char* argv[]) {
	/*
	 *
	verify number of command-line arguments == 4
  	process the command-line arguments:
    beats-per-minute
    time-signature (top)
    time-signature (bottom)

  implement main()
    device path (FQN): /dev/local/metronome
    create the metronome thread in-between calling resmgr_attach() and while(1) { ctp = dispatch_block(... }
	 */
	if (argc != 4) {
		printf("Invalid number of arguments. The program needs 4, but was given %d\n", argc);
		printf("Arguments needed: beats per minute, time signature(top), time-signature(bottom) ");
		exit(EXIT_FAILURE);
	}

	// from dispatch.h
	dispatch_t* dpp;
	// from dispatch.h
	resmgr_io_funcs_t io_funcs;
	// from dispatch.h
	resmgr_connect_funcs_t connect_funcs;
	// from iofunc.h
	iofunc_attr_t ioattr;
	// from dispatch.h
	dispatch_context_t   *ctp;
	int id;
	int time_sig_top;
	int time_sig_bottom;

	time_BPM = atoi(argv[1]);
	time_sig_top = atoi(argv[2]);
	time_sig_bottom = atoi(argv[3]);

	for(int i = 0; i < DTR_row_length; i++) {

		// if user's command line sig matches datarowtable, break;
		if(t[i].time_sig_top == time_sig_top && t[i].time_sig_bottom == time_sig_bottom) {
			// act as flag... stay -1 if failed
			counter = i;
			break;
		}
	}

	// if match after going through all the rows (checked flag). stay -1 if failed, goto exit
	if(counter >= 0) {
		// create the thread
		pthread_create(&thread_ID, NULL, &metronome_thread, NULL);
		// create the dispatcher
		if ((dpp = dispatch_create ()) == NULL) {
			fprintf (stderr,
					"%s:  Unable to allocate dispatch context.\n", argv [0]);
			return (EXIT_FAILURE);
		}

		iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);

		connect_funcs.open = io_open;
		io_funcs.read = io_read;
		io_funcs.write = io_write;

		iofunc_attr_init(&ioattr, S_IFCHR | 0666, NULL, NULL);

		if ((id = resmgr_attach (dpp, NULL, ATTACH_POINT, _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &ioattr)) == -1) {
			fprintf (stderr,
					"%s:  Unable to attach name.\n", argv [0]);
			return (EXIT_FAILURE);
		}


		ctp = dispatch_context_alloc(dpp);
		while(1) {
			ctp = dispatch_block(ctp);
			dispatch_handler(ctp);
		}

		return EXIT_FAILURE;
	}

	pthread_cancel(thread_ID);
	name_detach(attach, 0);

	return EXIT_SUCCESS;
}
