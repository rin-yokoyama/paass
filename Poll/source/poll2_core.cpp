// poll2_core.cpp

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <sstream>
#include <ctime>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "poll2_core.h"
#include "poll2_socket.h"

#include "CTerminal.h"

// Interface for the PIXIE-16
#include "PixieSupport.h"
#include "Utility.h"
#include "StatsHandler.hpp"
#include "Display.h"

#include "MCA_ROOT.h"
#include "MCA_DAMM.h"

// Values associated with the minimum timing between pixie calls (in us)
// Adjusted to help alleviate the issue with data corruption
#define END_RUN_PAUSE 100
#define POLL_TRIES 100
#define WAIT_TRIES 100

const std::string chan_params[21] = {"TRIGGER_RISETIME", "TRIGGER_FLATTOP", "TRIGGER_THRESHOLD", "ENERGY_RISETIME", "ENERGY_FLATTOP", "TAU", "TRACE_LENGTH",
									 "TRACE_DELAY", "VOFFSET", "XDT", "BASELINE_PERCENT", "EMIN", "BINFACTOR", "CHANNEL_CSRA", "CHANNEL_CSRB", "BLCUT",
									 "ExternDelayLen", "ExtTrigStretch", "ChanTrigStretch", "FtrigoutDelay", "FASTTRIGBACKLEN"};

std::vector<std::string> mod_params = {"MODULE_CSRA", "MODULE_CSRB", "MODULE_FORMAT", "MAX_EVENTS", "SYNCH_WAIT", "IN_SYNCH", "SLOW_FILTER_RANGE",
									"FAST_FILTER_RANGE", "MODULE_NUMBER", "TrigConfig0", "TrigConfig1", "TrigConfig2","TrigConfig3"};

MCA_args::MCA_args(){ Zero(); }
	
MCA_args::MCA_args(bool useRoot_, int totalTime_, std::string basename_){
	useRoot = useRoot_;
	totalTime = totalTime_;
	basename = basename_;
}

void MCA_args::Zero(){
	useRoot = false;
	totalTime = 0; // Needs to be zero for checking of MCA arguments
	basename = "MCA";
}

Poll::Poll(){
	pif = new PixieInterface("pixie.cfg");

	clock_vsn = 1000;

	// System flags and variables
	sys_message_head = " POLL: ";
	kill_all = false; // Set to true when the program is exiting
	start_acq = false; // Set to true when the command is given to start a run
	stop_acq = false; // Set to true when the command is given to stop a run
	record_data = false; // Set to true if data is to be recorded to disk
	do_reboot = false; // Set to true when the user tells POLL to reboot PIXIE
	force_spill = false; // Force poll2 to dump the current data spill
	acq_running = false; // Set to true when run_command is recieving data from PIXIE
	run_ctrl_exit = false; // Set to true when run_command exits
	do_MCA_run = false; // Set to true when the "mca" command is received
	raw_time = 0;

	// Run control variables
	boot_fast = false;
	insert_wall_clock = true;
	is_quiet = false;
	send_alarm = false;
	show_module_rates = false;
	zero_clocks = false;
	debug_mode = false;
	init = false;

	// Options relating to output data file
	output_directory = "./"; // Set with 'fdir' command
	output_filename = "pixie"; // Set with 'ouf' command
	output_title = "PIXIE data file"; // Set with 'htit' command
	output_run_num = 0; // Set with 'hnum' command
	output_format = 0; // Set with 'oform' command

	// The main output data file and related variables
	current_file_num = 0;
	current_filename = "";
	
	stats_interval = -1; //< in seconds
	histo_interval = -1; //< in seconds

	runDone = NULL;
	partialEventData = NULL;
	statsHandler = NULL;
	
	client = new Client();
}

bool Poll::initialize(){
	if(init){ return false; }

	// Set debug mode
	if(debug_mode){ 
		std::cout << sys_message_head << "Setting debug mode\n";
		output_file.SetDebugMode(); 
	}

	// Initialize the pixie interface and boot
	pif->GetSlots();
	if(!pif->Init()){ return false; }

	if(boot_fast){
		if(!pif->Boot(PixieInterface::DownloadParameters | PixieInterface::SetDAC | PixieInterface::ProgramFPGA)){ return false; } 
	}
	else{
		if(!pif->Boot(PixieInterface::BootAll)){ return false; }
	}

	// Check the scheduler
	Display::LeaderPrint("Checking scheduler");
	int startScheduler = sched_getscheduler(0);
	if(startScheduler == SCHED_BATCH){ std::cout << Display::InfoStr("BATCH") << std::endl; }
	else if(startScheduler == SCHED_OTHER){ std::cout << Display::InfoStr("STANDARD") << std::endl; }
	else{ std::cout << Display::WarningStr("UNEXPECTED") << std::endl; }

	if(!synch_mods()){ return false; }

	// Allocate memory buffers for FIFO
	n_cards = pif->GetNumberCards();
	
	// Two extra words to store size of data block and module number
	std::cout << "\nAllocating memory to store FIFO data (" << sizeof(word_t) * (EXTERNAL_FIFO_LENGTH + 2) * n_cards / 1024 << " kB)" << std::endl;
	
	// Allocate data for partial events
	std::cout << "Allocating memory for partial events (" << sizeof(eventdata_t) * n_cards / 1024 << " kB)" << std::endl;
	partialEventData = new eventdata_t[n_cards];
	for(size_t card = 0; card < n_cards; card++){
		partialEventWords.push_back(0);
		waitWords.push_back(0);
	}

	statsTime = 0;
	histoTime = 0;

	if(histo_interval != -1.){ 
		std::cout << "Allocating memory to store HISTOGRAM data (" << sizeof(PixieInterface::Histogram)*n_cards*pif->GetNumberChannels()/1024 << " kB)" << std::endl;
		for (unsigned int mod=0; mod < n_cards; mod++){
			for (unsigned int ch=0; ch < pif->GetNumberChannels(); ch++){
			  chanid_t id(mod, ch);
			  histoMap[id] = PixieInterface::Histogram();
			}
		}
	}

	runDone = new bool[n_cards];
	isExiting = false;

	waitCounter = 0;
	nonWaitCounter = 0;
	partialBufferCounter = 0;
	
	client->Init("127.0.0.1", 5555);
	
	return init = true;
}

Poll::~Poll(){
	if(init){
		close();
	}
}

bool Poll::close(){
	if(!init){ return false; }
	
	client->SendMessage((char *)"$KILL_SOCKET", 13);
	client->Close();
	
	// Just to be safe
	if(output_file.IsOpen()){ close_output_file(); }
	
	if(runDone){ delete[] runDone; }
	if(partialEventData){ delete[] partialEventData; }
	
	delete pif;
	
	init = false;
}

/* Safely close current data file if one is open. */
bool Poll::close_output_file(){
	if(output_file.IsOpen()){ // A file is already open and must be closed
		if(acq_running && record_data){
			std::cout << sys_message_head << "Warning! Attempted to close file while acquisition running.\n";
			return false;
		}
		else if(start_acq){
			std::cout << sys_message_head << "Warning! Attempted to close file while acquisition is starting.\n";
			return false;		
		}
		else if(stop_acq){
			std::cout << sys_message_head << "Warning! Attempted to close file while acquisition is stopping.\n";
			return false;			
		}
		std::cout << sys_message_head << "Closing output file.\n";
		client->SendMessage((char *)"$CLOSE_FILE", 12);
		output_file.CloseFile();
		return true;
	}
	std::cout << sys_message_head << "No file is open.\n";
	return true;
}

// Open an output file if needed
bool Poll::open_output_file(){
	if(!output_file.IsOpen()){ 
		if(!output_file.OpenNewFile(output_title, output_run_num, current_filename, output_directory)){
			std::cout << sys_message_head << "Failed to open output file! Check that the path is correct.\n";
			return record_data = false;
		}
		std::cout << sys_message_head << "Opening output file '" << current_filename << "'.\n";
	}
	else{ 
		std::cout << sys_message_head << "Warning! A file is already open. Close the current file before opening a new one.\n"; 
		return false;
	}
	
	return true;
}

bool Poll::synch_mods(){
	static bool firstTime = true;
	static char synchString[] = "IN_SYNCH";
	static char waitString[] = "SYNCH_WAIT";

	bool hadError = false;
	Display::LeaderPrint("Synchronizing");

	if(firstTime){
		// only need to set this in the first module once
		if(!pif->WriteSglModPar(waitString, 1, 0)){ hadError = true; }
		firstTime = false;
	}
	
	for(unsigned int mod = 0; mod < pif->GetNumberCards(); mod++){
		if (!pif->WriteSglModPar(synchString, 0, mod)){ hadError = true; }
	}

	if (!hadError){ std::cout << Display::OkayStr() << std::endl; }
	else{ std::cout << Display::ErrorStr() << std::endl; }

	return !hadError;
}

int Poll::write_data(word_t *data, unsigned int nWords){
	// Open an output file if needed
	if(!output_file.IsOpen()){
		open_output_file();
	}

	// Broadcast a spill notification to the network
	char packet[output_file.GetPacketSize()];
	int packet_size = output_file.BuildPacket(packet);
	client->SendMessage(packet, packet_size);

	// Handle the writing of buffers to the file
	return output_file.Write((char*)data, nWords);

	return -1;
}

/* Print help dialogue for POLL options. */
void Poll::help(){
	std::cout << "  Help:\n";
	std::cout << "   quit             - Close the program\n";
	std::cout << "   help (h)         - Display this dialogue\n";
	std::cout << "   version (v)      - Display Poll2 version information\n";
	std::cout << "   status           - Display system status information\n";
	std::cout << "   run (trun|start) - Start data acquisition and start recording data to disk\n";
	std::cout << "   stop             - Stop data acqusition and stop recording data to disk\n";	
	std::cout << "   startvme         - Start data acquisition\n";
	std::cout << "   stopvme          - Stop data acquisition\n";
	std::cout << "   tstop            - Stop recording data to disk\n";
	std::cout << "   reboot           - Reboot PIXIE crate\n";
	std::cout << "   force (hup)      - Force dump of current spill\n";
	std::cout << "   debug            - Toggle debug mode flag (default=false)\n";
	std::cout << "   quiet            - Toggle quiet mode flag (default=false)\n";
	std::cout << "   fdir [path]      - Set the output file directory (default='./')\n";
	std::cout << "   ouf [filename]   - Set the output filename (default='pixie.xxx')\n";
	std::cout << "   close (clo)      - Safely close the current data output file\n";
	std::cout << "   open             - Open a new data output file\n";
	std::cout << "   htit [title]     - Set the title of the current run (default='PIXIE Data File)\n";
	std::cout << "   hnum [number]    - Set the number of the current run (default=0)\n";
	std::cout << "   oform [0|1|2]    - Set the format of the output file (default=0)\n";
	std::cout << "   stats [time]     - Set the time delay between statistics dumps (default=-1)\n";
	std::cout << "   mca [root|damm] [time] [filename] - Use MCA to record data for debugging purposes\n";
	std::cout << "   dump [filename]                   - Dump pixie settings to file (default='Fallback.set')\n";
	std::cout << "   pread [mod] [chan] [param]        - Read parameters from individual PIXIE channels\n";
	std::cout << "   pmread [mod] [param]              - Read parameters from PIXIE modules\n";
	std::cout << "   pwrite [mod] [chan] [param] [val] - Write parameters to individual PIXIE channels\n";
	std::cout << "   pmwrite [mod] [param] [val]       - Write parameters to PIXIE modules\n";
	std::cout << "   adjust_offsets [module]           - Adjusts the baselines of a pixie module\n";
	std::cout << "   find_tau [module] [channel]       - Finds the decay constant for an active pixie channel\n";
	std::cout << "   toggle [module] [channel] [bit]   - Toggle any of the 19 CHANNEL_CSRA bits for a pixie channel\n";
	std::cout << "   toggle_bit [mod] [chan] [param] [bit] - Toggle any bit of any parameter of 32 bits or less\n";
	std::cout << "   csr_test [number]                 - Output the CSRA parameters for a given integer\n";
	std::cout << "   bit_test [num_bits] [number]      - Display active bits in a given integer up to 32 bits long\n";
}

/* Print help dialogue for reading/writing pixie channel parameters. */
void Poll::pchan_help(){
	std::cout << "  Valid Pixie16 channel parameters:\n";
	for(unsigned int i = 0; i < 21; i++){
		std::cout << "   " << chan_params[i] << "\n";
	}
}

/* Print help dialogue for reading/writing pixie module parameters. */
void Poll::pmod_help(){
	std::cout << "  Valid Pixie16 module parameters:\n";
	for(unsigned int i = 0; i < mod_params.size(); i++){
		std::cout << "   " << mod_params[i] << "\n";
	}
}

///////////////////////////////////////////////////////////////////////////////
// Poll::command_control
///////////////////////////////////////////////////////////////////////////////

/* Function to control the POLL command line interface */
void Poll::command_control(Terminal *poll_term_){
	char c;
	std::string cmd = "", arg;
	
#ifdef USE_NCURSES
	bool cmd_ready = true;
#else
	bool cmd_ready = false;
#endif
	
	while(true){
#ifdef USE_NCURSES
		cmd = poll_term_->GetCommand();
		if(cmd == "CTRL_D"){ cmd = "quit"; }
		else if(cmd == "CTRL_C"){ continue; }		
		poll_term_->print((cmd+"\n").c_str()); // This will force a write before the cout stream dumps to the screen
#else
		read(STDIN_FILENO, &c, 1);
		
		// check for system control commands
		if(c == '\004'){ break; } // ctrl + c
		else if(c == '\n' || c == '\r'){
			cmd_ready = true;
		}
		else if(c == '\033'){
			read(STDIN_FILENO, &c, 1); // skip the '['
			read(STDIN_FILENO, &c, 1);
		}
		else{ cmd += c; }
#endif

		if(cmd_ready){			
			if(cmd == ""){ continue; }
			
			size_t index = cmd.find(" ");
			if(index != std::string::npos){
				arg = cmd.substr(index+1, cmd.size()-index); // Get the argument from the full input string
				cmd = cmd.substr(0, index); // Get the command from the full input string
			}
			else{ arg = ""; }

			std::vector<std::string> arguments;
			unsigned int p_args = split_str(arg, arguments);
			
			// check for defined commands
			if(cmd == "quit" || cmd == "exit"){
				if(do_MCA_run){ std::cout << sys_message_head << "Warning! Cannot quit while MCA program is running\n"; }
				else if(acq_running){ std::cout << sys_message_head << "Warning! Cannot quit while acquisition running\n"; }
				else{
					kill_all = true;
					while(!run_ctrl_exit){ sleep(1); }
					break;
				}
			}
			else if(cmd == "kill"){
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Sending KILL signal\n";
					stop_acq = true; 
				}
				kill_all = true;
				while(!run_ctrl_exit){ sleep(1); }
				break;
			}
			else if(cmd == "help" || cmd == "h"){ help(); }
			else if(cmd == "version" || cmd == "v"){ 
				std::cout << "  Poll2 Core    v" << POLL2_CORE_VERSION << " (" << POLL2_CORE_DATE << ")\n"; 
				std::cout << "  Poll2 Socket  v" << POLL2_SOCKET_VERSION << " (" << POLL2_SOCKET_DATE << ")\n"; 
				std::cout << "  HRIBF Buffers v" << HRIBF_BUFFERS_VERSION << " (" << HRIBF_BUFFERS_DATE << ")\n"; 
				std::cout << "  CTerminal     v" << CTERMINAL_VERSION << " (" << CTERMINAL_DATE << ")\n";
			}
			else if(cmd == "status"){
				std::cout << "  Poll Run Status:\n";
				std::cout << "   Acq starting    - " << yesno(start_acq) << std::endl;
				std::cout << "   Acq stopping    - " << yesno(stop_acq) << std::endl;
				std::cout << "   Acq running     - " << yesno(acq_running) << std::endl;
				std::cout << "   Write to disk   - " << yesno(record_data) << std::endl;
				std::cout << "   File open       - " << yesno(output_file.IsOpen()) << std::endl;
				std::cout << "   Rebooting       - " << yesno(do_reboot) << std::endl;
				std::cout << "   Force Spill     - " << yesno(force_spill) << std::endl;
				std::cout << "   Run ctrl Exited - " << yesno(run_ctrl_exit) << std::endl;
				std::cout << "   Do MCA run      - " << yesno(do_MCA_run) << std::endl;

				std::cout << "\n  Poll Options:\n";
				std::cout << "   Boot fast   - " << yesno(boot_fast) << std::endl;
				std::cout << "   Wall clock  - " << yesno(insert_wall_clock) << std::endl;
				std::cout << "   Is quiet    - " << yesno(is_quiet) << std::endl;
				std::cout << "   Send alarm  - " << yesno(send_alarm) << std::endl;
				std::cout << "   Show rates  - " << yesno(show_module_rates) << std::endl;
				std::cout << "   Zero clocks - " << yesno(zero_clocks) << std::endl;
				std::cout << "   Debug mode  - " << yesno(debug_mode) << std::endl;
				std::cout << "   Initialized - " << yesno(init) << std::endl;
			}
			else if(cmd == "trun" || cmd == "run" || cmd == "start"){ // Tell POLL to start acq and start recording data to disk
				if(do_MCA_run){ std::cout << sys_message_head << "Warning! Cannot run acquisition while MCA program is running\n"; }
				else if(!acq_running){ start_acq = true; } // Start data acq
				else if(record_data){ std::cout << sys_message_head << "Acquisition is already running\n"; }
				
				record_data = true;
				if(!output_file.IsOpen()){ open_output_file();	}
			} 
			else if(cmd == "startvme"){ // Tell POLL to start data acquisition
				if(do_MCA_run){ std::cout << sys_message_head << "Warning! Cannot run acquisition while MCA program is running\n"; }
				else if(!acq_running){ start_acq = true; } // Start data acq
				else{ std::cout << sys_message_head << "Acquisition is already running\n"; }
			}
			else if(cmd == "tstop"){ // Tell POLL to stop recording to disk
				if(do_MCA_run){ stop_acq = true; }
				else if(!record_data || !acq_running){ std::cout << sys_message_head << "Not recording data to disk\n"; }
				else{ record_data = false; }
			}
			else if(cmd == "stop"){ // Tell POLL to stop recording data to disk and stop acq
				if(do_MCA_run){ stop_acq = true; }
				else if(!acq_running){ std::cout << sys_message_head << "Acquisition is not running\n"; }
				else{ stop_acq = true; }
				record_data = false;
			} 
			else if(cmd == "stopvme"){ // Tell POLL to stop data acquisition
				if(do_MCA_run){ stop_acq = true; }
				else if(!acq_running){ std::cout << sys_message_head << "Acquisition is not running\n"; }
				else{ stop_acq = true; }
			}
			else if(cmd == "reboot"){ // Tell POLL to attempt a PIXIE crate reboot
				if(do_MCA_run){ std::cout << sys_message_head << "Warning! Cannot reboot while MCA is running\n"; }
				else if(acq_running || do_MCA_run){ std::cout << sys_message_head << "Warning! Cannot reboot while acquisition running\n"; }
				else{ 
					do_reboot = true; 
					poll_term_->pause(do_reboot);
				}
			}
			else if(cmd == "clo" || cmd == "close"){ // Tell POLL to close the current data file
				if(do_MCA_run){ std::cout << sys_message_head << "Command not available for MCA run\n"; }
				else if(acq_running && record_data){ std::cout << sys_message_head << "Warning! Cannot close file while acquisition running\n"; }
				else{ close_output_file(); }
			}
			else if(cmd == "open"){ // Tell POLL to open a new data file
				if(do_MCA_run){ std::cout << sys_message_head << "Command not available for MCA run\n"; }
				else if(acq_running && record_data){ std::cout << sys_message_head << "Warning! Cannot open new file while acquisition running\n"; }
				else{ open_output_file(); }
			}
			else if(cmd == "hup" || cmd == "force"){ // Force spill
				if(do_MCA_run){ std::cout << sys_message_head << "Command not available for MCA run\n"; }
				else if(!acq_running){ std::cout << sys_message_head << "Acquisition is not running\n"; }
				else{ force_spill = true; }
			}
			else if(cmd == "debug"){ // Toggle debug mode
				if(debug_mode){
					std::cout << sys_message_head << "Toggling debug mode OFF\n";
					output_file.SetDebugMode(false);
					debug_mode = false;
				}
				else{
					std::cout << sys_message_head << "Toggling debug mode ON\n";
					output_file.SetDebugMode();
					debug_mode = true;
				}
			}
			else if(cmd == "quiet"){ // Toggle quiet mode
				if(is_quiet){
					std::cout << sys_message_head << "Toggling quiet mode OFF\n";
					is_quiet = false;
				}
				else{
					std::cout << sys_message_head << "Toggling quiet mode ON\n";
					is_quiet = true;
				}
			}
			else if(cmd == "fdir"){ // Change the output file directory
				if(arg != ""){
					output_directory = arg; 
					current_file_num = 0;
				
					// Append a '/' if the user did not include one
					if(*(output_directory.end()-1) != '/'){ output_directory += '/'; }
					std::cout << sys_message_head << "Set output directory to '" << output_directory << "'\n";
				}
				else{ std::cout << sys_message_head << "Using output directory '" << output_directory << "'\n"; }
				if(output_file.IsOpen()){ std::cout << sys_message_head << "New directory used for new files only! Current file is unchanged.\n"; }
			} 
			else if(cmd == "ouf"){ // Change the output file name
				if(arg != ""){
					output_filename = arg; 
					current_file_num = 0;
					output_file.SetFilenamePrefix(output_filename);
					std::cout << sys_message_head << "Set output filename to '" << output_filename << "'\n";
				}
				else{
					std::cout << sys_message_head << "Using output filename prefix '" << output_filename << "'\n";
					if(output_file.IsOpen()){ std::cout << sys_message_head << "Current output filename: " << output_file.GetCurrentFilename() << "\n"; }
				}
				if(output_file.IsOpen()){ std::cout << sys_message_head << "New filename prefix used for new files only! Current file is unchanged.\n"; }
			} 
			else if(cmd == "htit"){ // Change the title of the output file
				if(arg != ""){
					output_title = arg; 
					std::cout << sys_message_head << "Set run title to '" << output_title << "'\n";
				}
				else{ std::cout << sys_message_head << "Using output file title '" << output_title << "'\n"; }
				if(output_file.IsOpen()){ std::cout << sys_message_head << "New title used for new files only! Current file is unchanged.\n"; }
			} 
			else if(cmd == "hnum"){ // Change the run number to the specified value
				if(arg != ""){
					output_run_num = atoi(arg.c_str()); 
					std::cout << sys_message_head << "Set run number to '" << output_run_num << "'\n";
				}
				else{ std::cout << sys_message_head << "Using output file run number '" << output_run_num << "'\n"; }
				if(output_file.IsOpen()){ std::cout << sys_message_head << "New run number used for new files only! Current file is unchanged.\n"; }
			} 
			else if(cmd == "oform"){ // Change the output file format
				if(arg != ""){
					int format = atoi(arg.c_str());
					if(format == 0 || format == 1 || format == 2){
						output_format = atoi(arg.c_str());
						std::cout << sys_message_head << "Set output file format to '" << output_format << "'\n";
						if(output_format == 1){ std::cout << "  Warning! This output format is experimental and is not recommended for data taking\n"; }
						else if(output_format == 2){ std::cout << "  Warning! This output format is experimental and is not recommended for data taking\n"; }
						output_file.SetFileFormat(output_format);
					}
					else{ 
						std::cout << sys_message_head << "Unknown output file format ID '" << format << "'\n";
						std::cout << "  Available file formats include:\n";
						std::cout << "   0 - .ldf (HRIBF) file format (default)\n";
						std::cout << "   1 - .pld (PIXIE) file format (experimental)\n";
						std::cout << "   2 - .root file format (slow, not recommended)\n";
					}
				}
				else{ std::cout << sys_message_head << "Using output file format '" << output_format << "'\n"; }
				if(output_file.IsOpen()){ std::cout << sys_message_head << "New output format used for new files only! Current file is unchanged.\n"; }
			}
			else if(cmd == "stats"){
				if(arg != ""){
					stats_interval = atoi(arg.c_str());
					if(stats_interval > 0){ std::cout << sys_message_head << "Dumping statistics information every " << stats_interval << " seconds\n"; } // Stats are turned on
					else{ 
						std::cout << sys_message_head << "Disabling statistics output\n"; 
						stats_interval = -1;
					}
				}
				else if(stats_interval > 0){ std::cout << sys_message_head << "Dumping statistics information every " << stats_interval << " seconds\n"; }
				else{ std::cout << sys_message_head << "Statistics output is currently disabled\n"; }
			}
			else if(cmd == "mca" || cmd == "MCA"){ // Run MCA program using either root or damm
				if(do_MCA_run){
					std::cout << sys_message_head << "MCA program is already running\n\n";
					continue;
				}
				else if(acq_running){ 
					std::cout << sys_message_head << "Warning! Cannot run MCA program while acquisition is running\n\n";
					continue;
				}

				if (p_args >= 1) {
					std::string type = arguments.at(0);
					if(type == "root"){ mca_args.useRoot = true; }
					else if(type != "damm"){ mca_args.totalTime = atoi(type.c_str()); }
					if(p_args >= 2){
						if(mca_args.totalTime == 0){ mca_args.totalTime = atoi(arguments.at(1).c_str()); }
						else{ mca_args.basename = arguments.at(1); }
						if(p_args >= 3){ mca_args.basename = arguments.at(2); }
					}
				}
				if(mca_args.totalTime == 0){ 
					mca_args.totalTime = 10; 
					std::cout << sys_message_head << "Using default MCA time of 10 seconds\n";
				}
			
				do_MCA_run = true;
			}
			else if(cmd == "dump"){ // Dump pixie parameters to file
				std::ofstream ofile;
				
				if(p_args >= 1){
					ofile.open(arg.c_str());
					if(!ofile.good()){
						std::cout << sys_message_head << "Failed to open output file '" << arg << "'\n";
						std::cout << sys_message_head << "Check that the path is correct\n";
						continue;
					}
				}
				else{
					ofile.open("./Fallback.set");
					if(!ofile.good()){
						std::cout << sys_message_head << "Failed to open output file './Fallback.set'\n";
						continue;
					}
				}

				ParameterChannelDumper chanReader(&ofile);
				ParameterModuleDumper modReader(&ofile);

				// Channel dependent settings
				for(unsigned int param = 0; param < 21; param++){
					forChannel<std::string>(pif, -1, -1, chanReader, chan_params[param]);
				}

				// Channel independent settings
				for(unsigned int param = 0; param < mod_params.size(); param++){
					forModule(pif, -1, modReader, mod_params[param]);
				}

				if(p_args >= 1){ std::cout << sys_message_head << "Successfully wrote output parameter file '" << arg << "'\n"; }
				else{ std::cout << sys_message_head << "Successfully wrote output parameter file './Fallback.set'\n"; }
				ofile.close();
			}
			else if(cmd == "pwrite" || cmd == "pmwrite"){ // Write pixie parameters
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}
			
				if(cmd == "pwrite"){ // Syntax "pwrite <module> <channel> <parameter name> <value>"
					if(p_args > 0 && arguments.at(0) == "help"){ pchan_help(); }
					else if(p_args >= 4){
						int mod = atoi(arguments.at(0).c_str());
						int ch = atoi(arguments.at(1).c_str());
						double value = std::strtod(arguments.at(3).c_str(), NULL);
					
						ParameterChannelWriter writer;
						if(forChannel(pif, mod, ch, writer, make_pair(arguments.at(2), value))){ pif->SaveDSPParameters(); }
					}
					else{
						std::cout << sys_message_head << "Invalid number of parameters to pwrite\n";
						std::cout << sys_message_head << " -SYNTAX- pwrite [module] [channel] [parameter] [value]\n";
					}
				}
				else if(cmd == "pmwrite"){ // Syntax "pmwrite <module> <parameter name> <value>"
					if(p_args > 0 && arguments.at(0) == "help"){ pmod_help(); }
					else if(p_args >= 3){
						int mod = atoi(arguments.at(0).c_str());
						unsigned int value = (unsigned int)std::strtoul(arguments.at(2).c_str(), NULL, 0);
					
						ParameterModuleWriter writer;
						if(forModule(pif, mod, writer, make_pair(arguments.at(1), value))){ pif->SaveDSPParameters(); }
					}
					else{
						std::cout << sys_message_head << "Invalid number of parameters to pmwrite\n";
						std::cout << sys_message_head << " -SYNTAX- pmwrite [module] [parameter] [value]\n";
					}
				}
			}
			else if(cmd == "pread" || cmd == "pmread"){ // Read pixie parameters
				if(cmd == "pread"){ // Syntax "pread <module> <channel> <parameter name>"
					if(p_args > 0 && arguments.at(0) == "help"){ pchan_help(); }
					else if(p_args >= 3){
						int mod = atoi(arguments.at(0).c_str());
						int ch = atoi(arguments.at(1).c_str());
					
						ParameterChannelReader reader;
						forChannel(pif, mod, ch, reader, arguments.at(2));
					}
					else{
						std::cout << sys_message_head << "Invalid number of parameters to pread\n";
						std::cout << sys_message_head << " -SYNTAX- pread [module] [channel] [parameter]\n";
					}
				}
				else if(cmd == "pmread"){ // Syntax "pmread <module> <parameter name>"
					if(p_args > 0 && arguments.at(0) == "help"){ pmod_help(); }
					else if(p_args >= 2){
						int mod = atoi(arguments.at(0).c_str());
					
						ParameterModuleReader reader;
						forModule(pif, mod, reader, arguments.at(1));
					}
					else{
						std::cout << sys_message_head << "Invalid number of parameters to pmread\n";
						std::cout << sys_message_head << " -SYNTAX- pread [module] [parameter]\n";
					}
				}
			}
			else if(cmd == "adjust_offsets"){ // Run adjust_offsets
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}

				if(p_args >= 1){
					int mod = atoi(arguments.at(0).c_str());
					
					OffsetAdjuster adjuster;
					if(forModule(pif, mod, adjuster, 0)){ pif->SaveDSPParameters(); }
				}
				else{
					std::cout << sys_message_head << "Invalid number of parameters to adjust_offsets\n";
					std::cout << sys_message_head << " -SYNTAX- adjust_offsets [module]\n";
				}
			}
			else if(cmd == "find_tau"){ // Run find_tau
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}
			
				if(p_args >= 2){
					int mod = atoi(arguments.at(0).c_str());
					int ch = atoi(arguments.at(1).c_str());

					TauFinder finder;
					forChannel(pif, mod, ch, finder, 0);
				}
				else{
					std::cout << sys_message_head << "Invalid number of parameters to find_tau\n";
					std::cout << sys_message_head << " -SYNTAX- find_tau [module] [channel]\n";
				}
			}
			else if(cmd == "toggle"){ // Toggle a CHANNEL_CSRA bit
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}

				BitFlipper flipper;

				if(p_args >= 3){ 
					flipper.SetCSRAbit(arguments.at(2));
					
					std::string dum_str = "CHANNEL_CSRA";
					if(forChannel(pif, atoi(arguments.at(0).c_str()), atoi(arguments.at(1).c_str()), flipper, dum_str)){
						pif->SaveDSPParameters();
					}
				}
				else{
					std::cout << sys_message_head << "Invalid number of parameters to toggle\n";
					std::cout << sys_message_head << " -SYNTAX- toggle [module] [channel] [CSRA bit]\n\n";
					flipper.Help();				
				}
			}
			else if(cmd == "toggle_bit"){ // Toggle any bit of any parameter under 32 bits long
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}

				BitFlipper flipper;

				if(p_args >= 4){ 
					flipper.SetBit(arguments.at(3));
    
					if(forChannel(pif, atoi(arguments.at(0).c_str()), atoi(arguments.at(1).c_str()), flipper, arguments.at(2))){
						pif->SaveDSPParameters();
					}
				}
				else{
					std::cout << sys_message_head << "Invalid number of parameters to toggle_any\n";
					std::cout << sys_message_head << " -SYNTAX- toggle_any [module] [channel] [parameter] [bit]\n\n";
				}
			}
			else if(cmd == "csr_test"){ // Run CSRAtest method
				BitFlipper flipper;
				if(p_args >= 1){ flipper.CSRAtest((unsigned int)atoi(arguments.at(0).c_str())); }
				else{
					std::cout << sys_message_head << "Invalid number of parameters to csr_test\n";
					std::cout << sys_message_head << " -SYNTAX- csr_test [number]\n";
				}
			}
			else if(cmd == "bit_test"){ // Run Test method
				BitFlipper flipper;
				if(p_args >= 2){ flipper.Test((unsigned int)atoi(arguments.at(0).c_str()), std::strtoul(arguments.at(1).c_str(), NULL, 0)); }
				else{
					std::cout << sys_message_head << "Invalid number of parameters to bit_test\n";
					std::cout << sys_message_head << " -SYNTAX- bit_test [num_bits] [number]\n";
				}
			}
			else{ std::cout << sys_message_head << "Unknown command '" << cmd << "'\n"; }
			std::cout << std::endl;

#ifndef USE_NCURSES
			cmd = "";
			arg = "";
			cmd_ready = false;
#endif
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// Poll::run_control
///////////////////////////////////////////////////////////////////////////////

/// Function to control the gathering and recording of PIXIE data
void Poll::run_control(){
	//Number of words in the FIFO of each module.
	std::vector<word_t> nWords(n_cards);
	//Iterator to determine which card has the most words.
	std::vector<word_t>::iterator maxWords;
	//The FIFO storage array
	word_t *fifoData = new word_t[(EXTERNAL_FIFO_LENGTH + 2) * n_cards];

	const bool savePartialEvents = false;
	
	while(true){
		if(kill_all){ // Supersedes all other commands
			if(acq_running){ stop_acq = true; } // Safety catch
			else{ break; }
		}
		
		if(do_reboot){ // Attempt to reboot the PIXIE crate
			if(acq_running){ stop_acq = true; } // Safety catch
			else{
				std::cout << sys_message_head << "Attempting PIXIE crate reboot\n";
				pif->Boot(PixieInterface::BootAll);
				printf("Press any key to continue...");
				std::cin.get();
				do_reboot = false;
			}
		}

		if(do_MCA_run){ // Do an MCA run, if the acq is not running
			if(acq_running){ stop_acq = true; } // Safety catch
			else{
				if(mca_args.totalTime > 0.0){ std::cout << sys_message_head << "Performing MCA data run for " << mca_args.totalTime << " s\n"; }
				else{ std::cout << sys_message_head << "Performing infinite MCA data run. Type \"stop\" to quit\n"; }
				pif->RemovePresetRunLength(0);

				MCA *mca = NULL;
#if defined(USE_ROOT) && defined(USE_DAMM)
				if(mca_args.useRoot){ mca = new MCA_ROOT(pif, mca_args.basename.c_str()); }
				else{ mca = new MCA_DAMM(pif, mca_args.basename.c_str()); }
#elif defined(USE_ROOT)
				mca = new MCA_ROOT(pif, mca_args.basename.c_str());
#elif defined(USE_DAMM)
				mca = new MCA_DAMM(pif, mca_args.basename.c_str());
#endif

				if(mca && mca->IsOpen()){ mca->Run(mca_args.totalTime, &stop_acq); }
				mca_args.Zero();
				stop_acq = false;
				do_MCA_run = false;
				delete mca;
				
				std::cout << std::endl;
			}
		}

		//Start acquistion
		if (start_acq && !acq_running) {
			//Start list mode
			if(pif->StartListModeRun(LIST_MODE_RUN, NEW_RUN)){
				acq_running = true;
			}
			else{ 
				std::cout << sys_message_head << "Failed to start list mode run. Try rebooting PIXIE\n"; 
				acq_running = false;
			}
			start_acq = false;
		}
		else if (start_acq && acq_running) {
			std::cout << sys_message_head << "Already running!\n";
			start_acq = false;
		}

		if(acq_running){
			for (int i=0;i<(EXTERNAL_FIFO_LENGTH+2)*n_cards;i++) fifoData[i] = 0;

			//We loop until the FIFO has reached the threshold for any module
			for (unsigned int timeout = 0; timeout < POLL_TRIES; timeout++){ 
				//Check the FIFO size for every module
				for (short mod=0; mod < n_cards; mod++) {
					nWords[mod] = pif->CheckFIFOWords(mod);
				}
				//Find the maximum module
				maxWords = std::max_element(nWords.begin(), nWords.end());
				if(*maxWords > threshWords){ break; }
			}
			//Decide if we should read data based on threshold.
			bool readData = (*maxWords > threshWords || stop_acq);

			//We need to read the data out of the FIFO
			if (readData || force_spill) {
				force_spill = false;
				//Number of data words read from the FIFO
				size_t dataWords = 0;

				//Loop over each module's FIFO
				for (int mod=0;mod < n_cards; mod++) {
					//if (!is_quiet) std::cout << "Reading module: " << mod << "\n";
					
					//if the module has no words in the FIFO we continue to the next module
					if (nWords[mod] == 0) {
						// write an empty buffer if there is no data
						fifoData[dataWords++] = 2;
						fifoData[dataWords++] = mod;	    
						continue;
					}
					else if (nWords[mod] < 0) {
						std::cout << "Number of FIFO words less than 0 in module " << mod << std::endl;
						// write an empty buffer if there is no data
						fifoData[dataWords++] = 2;
						fifoData[dataWords++] = mod;	    
						continue;
					}
					
					//Check if the FIFO is overfilled
					bool fullFIFO = (nWords[mod] >= EXTERNAL_FIFO_LENGTH);
					if (fullFIFO) {
						std::cout << "Really full FIFO: Skipping module " << mod 
							<< " size: " << nWords[mod] << "/" 
							<< EXTERNAL_FIFO_LENGTH << std::endl;
						stop_acq = true;
						break;
					}

					//We write the number of words and the module into the FIFO array
					// This may be an initial guess if we have a partial event.
					fifoData[dataWords++] = nWords[mod] + 2;
					fifoData[dataWords++] = mod;

					//Try to read FIFO and catch errors.
					if(!pif->ReadFIFOWords(&fifoData[dataWords], nWords[mod], mod)){
						stop_acq = true;
						break;
					}

					//Print a message about what we did	
					if(!is_quiet) {
						std::cout << "Read " << nWords[mod] << " words from module " << mod << " to buffer position " << dataWords << std::endl;
					}

					//Previous poll parsed data to make sure it was not corrupted.
					//	This seems like something that should be done offline.
					//	We need to have a discussion about online validation.
					size_t parseWords = dataWords;
					//We declare the eventSize outside the loop in case there is a partial event.
					word_t eventSize = 0;
					while (parseWords < dataWords + nWords[mod]) {
						//Check first word to see if data makes sense.
						// Previous version then iterated the number of words forward and continued to parse.
						// We check the slot and event size.
						word_t slotRead = ((fifoData[dataWords] & 0xF0) >> 4);
						word_t slotExpected = pif->GetSlotNumber(mod);
						eventSize = ((fifoData[dataWords] & 0x7FFE2000) >> 17);
						bool virtualChannel = ((fifoData[parseWords] & 0x20000000) != 0);
						
						// Update the statsHandler with the event (for monitor.bash)
						if(!virtualChannel && statsHandler){ 
							word_t chanRead = (fifoData[parseWords] & 0xF);
							statsHandler->AddEvent(mod, chanRead, sizeof(word_t) * eventSize); 
						}

						if( slotRead != slotExpected ){ 
							std::cout << Display::ErrorStr() << " Slot read (" << slotRead 
								<< ") not the same as" << " slot expected (" 
								<< slotExpected << ")" << std::endl; 
							break;
						}
						else if(eventSize == 0){ 
							std::cout << "ZERO EVENT SIZE in mod " << mod << "!\n"; 
							break;
						}
						parseWords += eventSize;
					}

					//If parseWords is small then the parse failed for some reason
					if (parseWords < dataWords + nWords[mod]) {
						std::cout << Display::ErrorStr() << " Parsing indicated corrupted data at " << parseWords - dataWords << " words into FIFO.\n";

						//Print the first 100 words
						std::cout << std::hex;
						for(int i=0;i< 100;i++) {
							if (i%10 == 0) std::cout << std::endl << "\t";
							std::cout << fifoData[dataWords + i] << " ";
						}
						std::cout << std::dec << std::endl;

						stop_acq = true;
						break;
					}
					//Or we have too many words as an event was not completely pulled form the FIFO
					else if (parseWords > dataWords + nWords[mod]) {
						/*
	 					std::cout << "dataWords: " << dataWords;
						std::cout << " nWords: " << nWords[mod];
						std::cout << " parseWords: " << parseWords;
						std::cout << " eventSize " << eventSize << std::endl;
						*/
						std::cout << "Partial event!\n";
						word_t missingWords = parseWords - dataWords - nWords[mod];
						word_t partialSize = eventSize - missingWords;
	
						std::cout << std::hex;
						for(int i=0;i< partialSize;i++) {
							if (i%10 == 0) std::cout << std::endl << "\t";
							std::cout << fifoData[parseWords - eventSize + i] << " ";
						}
						std::cout << std::dec << std::endl;

						//std::cout << "Getting remaing partial event of " << missingWords << "/" << eventSize << " words!\n";
						std::cout << "Getting remaing partial event of " << missingWords << "+" << partialSize << "=" << eventSize << " words!\n";
						word_t partialEventBuffer[EXTERNAL_FIFO_LENGTH] = {0};
						//We wait until the words are available
						while (pif->CheckFIFOWords(mod) < missingWords+9) {
							std::cout << Display::WarningStr("Waiting for words ") << missingWords << std::endl;
							usleep(15);
						}
						if (!pif->ReadFIFOWords(&fifoData[dataWords + nWords[mod]], missingWords, mod)) {
						//if (!pif->ReadFIFOWords(partialEventBuffer, missingWords, mod)) {
							stop_acq = true;
							break;
						}

						/*
						std::cout << std::hex;
						for(int i=0;i< missingWords;i++) {
							if (i%10 == 0) std::cout << std::endl << "\t";
							//std::cout << fifoData[dataWords + nWords[mod] + i] << " ";
							std::cout << partialEventBuffer[i] << " ";
						}
						std::cout << std::dec << std::endl;
						*/

						std::cout << "total event:\n";
						std::cout << std::hex;
						for(int i=0;i< eventSize+1;i++) {
							if (i%10 == 0) std::cout << std::endl << "\t";
							std::cout << fifoData[parseWords - eventSize + i] << " ";
						}
						std::cout << std::dec << std::endl;

						//Reassign the first word of spill to new length
						nWords[mod] += missingWords;
						fifoData[dataWords - 2] = nWords[mod] + 2;
					
					}
					//The data should be good so we iterate the position in the storage array.
					dataWords += nWords[mod];
				} //End loop over modules for reading FIFO

				//We have read the FIFO now we write the data	
				if(record_data){ 
					write_data(fifoData, dataWords); 
				}
				
				// Add time to the statsHandler (for monitor.bash)
				if(statsHandler){ statsHandler->AddTime(durSpill * 1e-6); }
			} //If we had exceeded the threshold or forced a flush

			//Handle a stop signal
			if(stop_acq){ 
				//time(&raw_time);
				//time_info = localtime(&raw_time);
				//std::cout << sys_message_head << "Stopping run at " << asctime(time_info);
				pif->EndRun();
				
				//Reset status flags
				stop_acq = false;
				acq_running = false;

				//Sleep for a bit to allow the modules to finish up
				usleep(END_RUN_PAUSE);	
				
				// Check if each module has ended its run properly.
				for(size_t mod = 0; mod < n_cards; mod++){
					if(!pif->CheckRunStatus(mod)){
						runDone[mod] = true;
						std::cout << "Run ended in module " << mod << std::endl;
					}
					else {
						std::cout << "Run not properly finished in module " << mod << std::endl;
					}
				}
				std::cout << std::endl;			
			} //End of handling a stop acq flag
		}
	}

	delete[] fifoData;
	run_ctrl_exit = true;
}

///////////////////////////////////////////////////////////////////////////////
// Support Functions
///////////////////////////////////////////////////////////////////////////////

unsigned int split_str(std::string str_, std::vector<std::string> &args, char delimiter_){
	args.clear();
	std::string temp = "";
	unsigned int count = 0;
	for(unsigned int i = 0; i < str_.size(); i++){
		if(str_[i] == delimiter_ || i == str_.size()-1){
			if(i == str_.size()-1){ temp += str_[i]; }
			args.push_back(temp);
			temp = "";
			count++;
		}
		else{ temp += str_[i]; }		
	}
	return count;
}

/* Pad a string with '.' to a specified length. */
std::string pad_string(const std::string &input_, unsigned int length_){
	std::string output = input_;
	for(unsigned int i = input_.size(); i <= length_; i++){
		output += '.';
	}
	return output;
}

std::string yesno(bool value_){
	if(value_){ return "Yes"; }
	return "No";
}
