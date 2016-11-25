#include <alsa/asoundlib.h>
#include <sys/select.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <cerrno>
#include <cmath>


using namespace std;
using namespace std::chrono;

int main()
{

    snd_pcm_t *pcm_handle;
	
	//Don't use alloca because ewwww, that's why.
	vector<char> hw_params_data(snd_pcm_hw_params_sizeof());
	snd_pcm_hw_params_t *hwparams = (snd_pcm_hw_params_t*) hw_params_data.data();
	
	//hw:0,0 is first hardware device, first output.
	//plughw:0,0 allows software resampling, etc.
	const char* device_name = "hw:0,0";
	
	//Open a stream for playback.
	if(snd_pcm_open(&pcm_handle, device_name, SND_PCM_STREAM_PLAYBACK, 0) < 0)
	{
		cerr << "Error opening " << device_name<< endl;
		return 1;
	}


	//Read all the possible parameter combos from the device.
	if(snd_pcm_hw_params_any(pcm_handle, hwparams) < 0)
	{
		cerr << "Error reading configuration " << endl;
		return 1;
	}

	//Now, we have to set up the hardware. We fill hwparams, then send that to the device.


	//Interleaved data: l r l r, not lllll rrrrr
	//Interleaved is simpler.
	if (snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) 
	{
		cerr << "Error setting access" << device_name << endl;
		return 1;
	}

	//Signed, 16 bit, little endian data.
	if (snd_pcm_hw_params_set_format(pcm_handle, hwparams, SND_PCM_FORMAT_S16_LE) < 0) 
	{
		cerr << "Error setting sample format" << device_name<<endl;
		return 1;
	}
	
	//We might not get the rate we want, so ask for one and see what we get.
	unsigned int rate = 44100;
	unsigned int exact_rate = rate;
	if(snd_pcm_hw_params_set_rate_near(pcm_handle, hwparams, &exact_rate, nullptr) < 0)
	{
		cerr << "Error setting rate to " << rate << " "<< device_name<< ": " << strerror(errno) << endl;
		return 1;
	}
	
	cerr << "Requested rate: " << rate << " actual rate: " << exact_rate << endl;

	//Stereo: lots of hardware only offers stereo now.
    if (snd_pcm_hw_params_set_channels(pcm_handle, hwparams, 2) < 0) 
	{
		cerr << "Error setting channels "<< device_name<< ": " << strerror(errno) << endl;
		return 1;
    }

	//We're now on 16 bit stereo data
	//
	//A frame is a single complete sample, so for stereo 16 bits, it's 4 bytes.
	const int frame_size=4;
	
	//A period is the number of frames before each interrupt. select/poll/epoll
	//will return when one period has been played.
	//
	//A buffer contains at least one period of data. The larger the buffer, the longer
	//you have to fill it before an underrun.

	snd_pcm_uframes_t period = 8192;  //about 0.18 seconds at 44100 Hz
	if(snd_pcm_hw_params_set_period_size(pcm_handle, hwparams, period, 0) < 0)
	{
		cerr << "Error setting period to " << period << " "<< device_name<< ": " << strerror(errno) << endl;
		return 1;
	}

	//Go for double buffering. That means, we get an interrupt when it starts playing the 
	//first period in the buffer. We then generate and append exactly one period ASAP 
	//after getting the interrupt
	snd_pcm_uframes_t buffer_frames = period*2;

	if(snd_pcm_hw_params_set_buffer_size(pcm_handle, hwparams, buffer_frames) < 0)
	{
		cerr << "Error setting buffer to " << buffer_frames << " "<< device_name<< ": " << strerror(errno) << endl;
		return 1;
	}


	//Now actually apply the parameters
	if (snd_pcm_hw_params(pcm_handle, hwparams) < 0) 
	{
		cerr << "Error setting parameters: " << device_name << endl;
		return 1;
	}
	


	//Get the file descriptors. alsa thinks we want to use poll, so it
	//returns then in poll ready format.
	cerr << "File descriptors: " <<  snd_pcm_poll_descriptors_count(pcm_handle) << endl;

	struct pollfd fd;
	if(snd_pcm_poll_descriptors(pcm_handle, &fd, 1) < 0)
	{
		cerr << "Error getting descriptor for " << device_name << endl;
		return 1;
	}
	
	cerr << "FD = " << fd.fd << endl;
	cerr << "  POLLIN: " << !!(fd.events&POLLIN) << endl;
	cerr << "  POLLOUT: " << !!(fd.events&POLLOUT) << endl;
	cerr << "  POLLRDHUP: " << !!(fd.events&POLLRDHUP) << endl;
	cerr << "  POLLHUP: " << !!(fd.events&POLLHUP) << endl;

	//Some code to update a data buffer with a sine wave that
	//sweeps left to right.
	//Have one period in the buffer.
	vector<char> data(period* frame_size);
	double freq = 1000;
	double lr_freq = 1;
	uint64_t timepoint = 0;

	auto generate_next_data = [&](){
		for(unsigned int i=0; i < period; i++, timepoint++)
		{
			double t = 1.0 * timepoint / exact_rate;

			double vd = sin(t * M_PI * 2 * freq);
			double lr = (sin(t * M_PI*2*lr_freq)+1)/2;
			
			int16_t vul = ((vd*lr)*32768) ;
			int16_t vur = ((vd*(1-lr))*32768) ;

			data[i*frame_size + 0] = vul & 0xff;
			data[i*frame_size + 1] = (vul>>8) & 0xff;
			data[i*frame_size + 2] = vur & 0xff;
			data[i*frame_size + 3] = (vur>>8) & 0xff;
		}
	};

	generate_next_data();
	
	auto now = std::chrono::system_clock::now();

	//Writing data starts the stream.
	cerr << snd_pcm_writei(pcm_handle, data.data(), period) << endl;

	
	for(;;)
	{
		//Wait for an interrupt using select() just to be perverse
		//and to prove it's just an FD and you don't have to use poll

		fd_set wr_set;
		FD_ZERO(&wr_set);
		FD_SET(fd.fd, &wr_set);

		int r = select(FD_SETSIZE, 0, &wr_set, 0, 0);
		//Print out a timestamp. Note there should be two in quick succession to fill the buffer
		//then the spacing should be as expected.
		cerr << r << "  FDs ready ";
		cerr << duration_cast<duration<double>>(system_clock::now() - now).count();


		generate_next_data();
		int ret = snd_pcm_writei(pcm_handle, data.data(), period);

		if(ret <0)
		{
			//If an underrun happens, we need to "prepare" the stream.
			snd_pcm_prepare(pcm_handle);
			cerr << "oops\n";
		}
		cerr << "...\n";
	}













}

