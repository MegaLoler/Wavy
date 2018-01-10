/* TODO:
 *   variable audio format (not hardcoded to 16bit mono)
 *   hardware accelerate the waveform rendering
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>

#define max(a, b)				\
  ({ __typeof__ (a) _a = (a);			\
    __typeof__ (b) _b = (b);			\
    _a > _b ? _a : _b; })

// constants
#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 200
#define MAX_SAMPLES 1024 * 1024 * 64

// global vars
struct cliArgs cliArgs; // to hold the cli args
struct audioBuffer audioBuffer; // to hold the loaded audio
struct region viewport; // currently viewed portion
SDL_Window* mainWindow; // main window
SDL_Surface* mainSurface; // surface of the main window

// a structure to represent a region of audio
struct region
{
  int start; // leftmost sample (inclusive)
  int stop; // rightmost sample (exclusive)
};

// a structure to hold audio data
struct audioBuffer
{
  uint16_t* buffer;
  int length;
};

// structure to hold the cli args
struct cliArgs
{
  const char* filename;
  int autoplay;
  int autoloop;
};

// load a cli arg struct with actual cli args
int loadCliArgs(struct cliArgs* cliArgs, int argc, const char* argv[])
{
  // loop through the arg strings
  int i;
  // skip first one because thats the name of the command
  for(i = 1; i < argc; i++)
    {
      const char* arg = argv[i];
      
      // auto loop
      if(strcmp(arg, "-l") == 0)
	cliArgs->autoloop = 1;
      // no auto loop
      else if(strcmp(arg, "-nl") == 0)
	cliArgs->autoloop = 0;
      // auto play
      else if(strcmp(arg, "-p") == 0)
	cliArgs->autoplay = 1;
      // no auto play
      else if(strcmp(arg, "-np") == 0)
	cliArgs->autoplay = 0;
      // filename
      else
	cliArgs->filename = arg;
    }

  // all was good
  return 0;
}

// handle the cli args
int processCommandLineArgs(int argc, const char* argv[])
{
  // the default values
  cliArgs.filename = NULL;
  cliArgs.autoplay = 1;
  cliArgs.autoloop = 1;

  // load values from cli
  return loadCliArgs(&cliArgs, argc, argv);
}

// setup sdl stuff
int initSDL()
{
  // initialize window and surface pointers
  mainWindow = NULL;
  mainSurface = NULL;
  
  // setup sdl subsystems
  if(SDL_Init(SDL_INIT_VIDEO) < 0)
    {
      fprintf(stderr, "Error initializing SDL subsystems!\n");
      return -1;
    }

  // attempt to create the main window
  mainWindow = SDL_CreateWindow("Wavy",
				SDL_WINDOWPOS_UNDEFINED,
				SDL_WINDOWPOS_UNDEFINED,
				WINDOW_WIDTH, WINDOW_HEIGHT,
				SDL_WINDOW_SHOWN);
  if(mainWindow == NULL)
    {
      fprintf(stderr, "Error creating main window!\n");
      return -1;
    }

  // grab the main window's surface
  mainSurface = SDL_GetWindowSurface(mainWindow);

  // all was good
  return 0;
}

// cleanup sdl before exit
void cleanupSDL()
{
  // destroy the main window and quit sdl subsystems
  SDL_DestroyWindow(mainWindow);
  SDL_Quit();
}

// calculate the sum of the squares in a range of values
double sumOfSquares(int offset, int length, uint16_t* array)
{
  int i;
  double sum = 0;
  for(i = offset; i < offset + length; i++)
    {
      double value = array[i];
      sum += value * value;
    }
  return sum;
}

// calculate the root mean square of a range of values in an array
double rootMeanSquare(int offset, int length, uint16_t* array)
{
  return sqrt(sumOfSquares(offset, length, array) / length);
}

// draw a waveform on an sdl surface given a viewport
void drawWaveform(SDL_Surface* surface, struct audioBuffer buffer, struct region viewport)
{
  // get dimensions for conveniences
  int width = surface->w;
  int height = surface->h;

  // viewport stuff
  int viewportStartSample = viewport.start;
  int viewportEndSample = viewport.stop;
  int sampleRange = viewportEndSample - viewportStartSample;
  float samplesPerPixel = 1.0 * sampleRange / width;
  int samplePeak = UINT16_MAX;

  // draw each column
  int i;
  for(i = 0; i < width; i++)
    {
      // this is the sample percentage and pixel conversions
      int sampleIndex = viewportStartSample + i * samplesPerPixel;
      float samplePercent = rootMeanSquare(sampleIndex, max(1, samplesPerPixel), buffer.buffer) / samplePeak;
      int filledHeight = height * samplePercent;
      int unfilledHeight = height - filledHeight;
      
      // get rects of this column
      SDL_Rect filledRect = { i, unfilledHeight, 1, filledHeight };
      SDL_Rect unfilledRect = { i, 0, 1, unfilledHeight };

      // get the fill colors
      Uint32 filledColor = SDL_MapRGB(surface->format, 255, 0, 0);
      Uint32 unfilledColor = SDL_MapRGB(surface->format, 0, 0, 0);

      // fill this column
      SDL_FillRect(surface, &filledRect, filledColor);
      SDL_FillRect(surface, &unfilledRect, unfilledColor);
    }
}

// the main sdl gui loop
void mainLoop()
{
  while(1)
    {
      // this is all just temp stuff
      drawWaveform(mainSurface, audioBuffer, viewport);
      SDL_UpdateWindowSurface(mainWindow);
      SDL_Delay(10);
    }
}

// load an audio file into a buffer using ffmpeg
struct audioBuffer loadAudioFromFile(const char* filename)
{
  // create a buffer to store the data
  uint16_t* buffer = (uint16_t*)calloc(MAX_SAMPLES, sizeof(uint16_t));
  struct audioBuffer audioBuffer = { buffer, 0 };

  // load the raw data from ffmpeg
  // for now just force mono and 16bit
  FILE* pipe;
  pipe = popen("ffmpeg -hide_banner -loglevel panic -i test.mp3 -f s16le -ac 1 -", "r");
  audioBuffer.length = fread(buffer, sizeof(uint16_t), MAX_SAMPLES, pipe);
  pclose(pipe);

  // return the buffer struct
  return audioBuffer;
}

// load the given audio file from the cli
int loadAudio()
{
  // make sure a filename was given
  if(cliArgs.filename == NULL)
    {
      fprintf(stderr, "Please provide a filename!\n");
      return -1;
    }
  
  // load the audio into the buffer
  audioBuffer = loadAudioFromFile(cliArgs.filename);
  if(audioBuffer.length == 0) return -1;

  // set the viewport to show the whole file
  viewport.start = 0;
  viewport.stop = audioBuffer.length;
  
  // it all went well
  return 0;
}

// main program starts here!
int main(int argc, const char* argv[])
{
  // first handle the cli args
  if(processCommandLineArgs(argc, argv))
    {
      fprintf(stderr, "Invalid command line arguments!\n");
      return -1;
    }

  // then setup sdl
  if(initSDL())
    {
      fprintf(stderr, "Error initializing SDL: %s\n",
	      SDL_GetError());
      return -1;
    }

  // load the audio file
  if(loadAudio())
    {
      fprintf(stderr, "Error loading the audio file!\n");
      return -1;
    }

  // enter main loop
  mainLoop();

  // cleanup sdl
  cleanupSDL();

  // we made it woohoo
  return 0;
}
