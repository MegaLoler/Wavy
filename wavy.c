/* TODO:
 *   variable audio format (not hardcoded to 16bit mono)
 *   hardware accelerate the waveform rendering
 *   dynamic allocate audio buffer rather than set max
 *   only rerender damaged areas
 *   drop redundant ui events? like mouse motion?
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>

#define min(a, b)				\
  ({ __typeof__ (a) _a = (a);			\
    __typeof__ (b) _b = (b);			\
    _a <_b ? _a : _b; })
#define max(a, b)				\
  ({ __typeof__ (a) _a = (a);			\
    __typeof__ (b) _b = (b);			\
    _a > _b ? _a : _b; })

// constants
#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 200
#define MAX_SAMPLES 1024 * 1024 * 64

// enum for abstract user input target
enum target
  {
    PLAY,
    REGION,
    VIEWPORT
  };

// enum for abstract user input primary or secondary action
enum action
  {
    PRIMARY,
    SECONDARY
  };

// global vars
struct cliArgs cliArgs; // to hold the cli args
struct audioBuffer audioBuffer; // to hold the loaded audio

// user input targets
int playPosition; // current sample
struct region selection; // currently selected portion
struct region viewport; // currently viewed portion of audio
enum action selectionGrabbedPole; // currently grabbed end

// sdl resources
SDL_Window* mainWindow; // main window
SDL_Surface* mainSurface; // surface of the main window

// a structure representing which modifier keys are held
struct modifiers
{
  int ctrl;
  int alt;
  int shift;
};

// a structure to represent the primary and secondary values of a user input target
struct targetValues
{
  int primary;
  int secondary;
};

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
				SDL_WINDOW_SHOWN |
				SDL_WINDOW_RESIZABLE);
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

// whether a value is in a range
int inRange(int value, int a, int b)
{
  int min = min(a, b);
  int max = max(a, b);
  return min <= value && value < max;
}

// whether a sample position is inside the user selection
int inSelection(int position)
{
  return inRange(position, selection.start, selection.stop);
}

// whether a selection currently exists
int selectionExists()
{
  // selection of length 0 = no selection
  return selection.start != selection.stop;
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
  float minSamplesPerPixel = max(1, samplesPerPixel);
  int samplePeak = UINT16_MAX;

  // draw each column
  int i;
  for(i = 0; i < width; i++)
    {
      // the sample index at this pixel
      int sampleIndex = viewportStartSample + i * samplesPerPixel;

      // draw audio cursor here if play position is here
      if(inRange(playPosition, sampleIndex, sampleIndex + minSamplesPerPixel))
	{
	  // get rect of this column
	  SDL_Rect rect = { i, 0, 1, height };

	  // get the fill color
	  Uint32 color = SDL_MapRGB(surface->format, 255, 255, 255);

	  // fill this column
	  SDL_FillRect(surface, &rect, color);
	}
      else
	{
	  // this is the sample percentage and pixel conversions
	  float samplePercent = rootMeanSquare(sampleIndex, minSamplesPerPixel, buffer.buffer) / samplePeak;
	  int filledHeight = height * samplePercent;
	  int unfilledHeight = height - filledHeight;
      
	  // get rects of this column
	  SDL_Rect filledRect = { i, unfilledHeight, 1, filledHeight };
	  SDL_Rect unfilledRect = { i, 0, 1, unfilledHeight };

	  // get the fill colors
	  Uint32 filledColor;
	  // the appropriate waveform color depends on whether its in the user selected region or not
	  if(inSelection(sampleIndex))
	    filledColor = SDL_MapRGB(surface->format, 255, 255, 0);
	  else
	    filledColor = SDL_MapRGB(surface->format, 255, 0, 0);
	  Uint32 unfilledColor = SDL_MapRGB(surface->format, 0, 0, 0);

	  // fill this column
	  SDL_FillRect(surface, &filledRect, filledColor);
	  SDL_FillRect(surface, &unfilledRect, unfilledColor);
	}
    }
}

// used to draw the screen when something changes
// todo: only update within a damaged rect ?
void redrawScreen()
{
  // this is all just temp stuff
  drawWaveform(mainSurface, audioBuffer, viewport);
  SDL_UpdateWindowSurface(mainWindow);
}

// see which modifiers are currently held down
struct modifiers getModifiers()
{
  const Uint8* state = SDL_GetKeyboardState(NULL);
  struct modifiers modifiers = { state[SDL_SCANCODE_LCTRL] ||
				 state[SDL_SCANCODE_RCTRL],
				 state[SDL_SCANCODE_LALT] ||
				 state[SDL_SCANCODE_RALT],
				 state[SDL_SCANCODE_LSHIFT] ||
				 state[SDL_SCANCODE_LSHIFT]};
  return modifiers;
}

// get the final target of an abstract user event
enum target getFinalTarget(enum target target)
{
  // let the target be overridable by modifiers
  struct modifiers modifiers = getModifiers();
  if(modifiers.ctrl && modifiers.alt)
    return PLAY;
  else if(modifiers.ctrl && !modifiers.alt)
    return REGION;
  else if(!modifiers.ctrl && modifiers.alt)
    return VIEWPORT;
  // other wise it stays unchanged
  else return target;
}

// set the values of an abstract user input target
void setTargetValues(enum target target, struct targetValues values)
{
  switch(target)
    {
    case PLAY:
      // set the audio cursor position
      playPosition = values.primary;
      break;
    case REGION:
      // set the selected region of audio
      selection.start = values.primary;
      selection.stop = values.secondary;
      break;
    case VIEWPORT:
      // set the viewport region
      viewport.start = values.primary;
      viewport.stop = values.secondary;
      break;
    }
  
  // show the changes on the screen
  redrawScreen();
}

// get the values of an abstract user input target
struct targetValues getTargetValues(enum target target)
{
  // the return values will go in here
  struct targetValues values = { 0, 0 };

  // get the values for the given input target
  switch(target)
    {
    case PLAY:
      // get the audio cursor position
      values.primary = playPosition;
      values.secondary = playPosition;
      break;
    case REGION:
      // get the selected region of audio
      values.primary = selection.start;
      values.secondary = selection.stop;
      break;
    case VIEWPORT:
      // get the viewport region
      values.primary = viewport.start;
      values.secondary = viewport.stop;
      break;
    }
  
  // return the values
  return values;
}

// set specifically the primary value of a target
void setTargetPrimaryValue(enum target target, int value)
{
  struct targetValues old = getTargetValues(target);
  struct targetValues new = { value, old.secondary };
  setTargetValues(target, new);
}

// set specifically the secondary value of a target
void setTargetSecondaryValue(enum target target, int value)
{
  struct targetValues old = getTargetValues(target);
  struct targetValues new = { old.primary, value };
  setTargetValues(target, new);
}

// set both values of a target the same
void setTargetBothValues(enum target target, int value)
{
  struct targetValues new = { value, value };
  setTargetValues(target, new);
}

// set either primary or secondary value of target
void setTargetValue(enum target target, int value, enum action which)
{
  // set whichever one it is
  switch(which)
    {
    case PRIMARY:
      setTargetPrimaryValue(target, value);
      break;
    case SECONDARY:
      setTargetSecondaryValue(target, value);
      break;
    }
}

// get the sample index at the mouse position
int getMouseSamplePosition(SDL_Event event)
{
  // get pixel coordinates
  int width = mainSurface->w;
  int x = event.motion.x;

  // viewport stuff
  int viewportStartSample = viewport.start;
  int viewportEndSample = viewport.stop;
  int sampleRange = viewportEndSample - viewportStartSample;
  float samplesPerPixel = 1.0 * sampleRange / width;
  int sampleIndex = viewportStartSample + x * samplesPerPixel;

  // return the sample indexd
  return sampleIndex;
}

enum action getNearestSelectionPole(int position)
{
  // get the distances between both poles
  int primaryDelta = abs(selection.start - position);
  int secondaryDelta = abs(selection.stop - position);

  // return whichever is closest
  if(primaryDelta < secondaryDelta)
    return PRIMARY;
  else return SECONDARY;
}

// initiate a region selection
void initiateSelection(int position)
{
  // if shift is held then modify existing selection
  struct modifiers modifiers = getModifiers();
  if(modifiers.shift)
    {
      selectionGrabbedPole = getNearestSelectionPole(position);
    }
  // otherwise grab the nearest pole
  else
    {
      // clear the selection
      setTargetBothValues(REGION, position);

      // grab the secondary pole
      selectionGrabbedPole = SECONDARY;
    }
}

// continue a region selection
void continueSelection(int position)
{
  // set the position of whichever selection pole is grabbed
  setTargetValue(REGION, position, selectionGrabbedPole);
}

// unselect anything
void cancelSelection()
{
  setTargetBothValues(REGION, 0);
}

// deal with mouse drags
void mouseDrag(SDL_Event event, enum target target)
{
  // override with modifiers
  target = getFinalTarget(target);

  // do different things depending on the target
  switch(target)
    {
    case PLAY:
      // set the player position
      setTargetPrimaryValue(target, getMouseSamplePosition(event));
      break;
    case REGION:
      // continue a selection
      continueSelection(getMouseSamplePosition(event));
      break;
    case VIEWPORT:
      // clicking on viewport does nothing
      break;
    }
}

// handle window events
void handleWindowEvent(SDL_Event event)
{
  switch(event.window.event)
    {
    case SDL_WINDOWEVENT_SIZE_CHANGED:
      mainSurface = SDL_GetWindowSurface(mainWindow);
    case SDL_WINDOWEVENT_EXPOSED:
      redrawScreen();
      break;
    }
}

// handle key events
void handleKeyboardEvent(SDL_Event event)
{
}

// handle mouse move events
void handleMouseMotionEvent(SDL_Event event)
{
  // this is for dragging events really
  // so make appropriate mouse drag events out of this
  int state = event.motion.state;
  if(state & SDL_BUTTON_LMASK)
    mouseDrag(event, PLAY);
  if(state & SDL_BUTTON_RMASK)
    mouseDrag(event, REGION);
  if(state & SDL_BUTTON_MMASK)
    mouseDrag(event, VIEWPORT);
}

// handle mouse button events
void handleMouseButtonEvent(SDL_Event event)
{
  // ignore mouse ups
  if(event.button.state == SDL_PRESSED)
    {
      // first set the target of the action by which button
      enum target target;
      switch(event.button.button)
	{
	case SDL_BUTTON_LEFT:
	  target = PLAY;
	  break;
	case SDL_BUTTON_RIGHT:
	  target = REGION;
	  break;
	case SDL_BUTTON_MIDDLE:
	  target = VIEWPORT;
	  break;
	}

      // and then let it be modified with modifiers
      target = getFinalTarget(target);
      // do different things depending on the target
      switch(target)
	{
	case PLAY:
	  // set the play position
	  setTargetPrimaryValue(target, getMouseSamplePosition(event));
	  break;
	case REGION:
	  // initiate a selection
	  initiateSelection(getMouseSamplePosition(event));
	  break;
	case VIEWPORT:
	  // clicking on viewport does nothing
	  break;
	}
    }
}

// handle mouse wheel events
void handleMouseWheelEvent(SDL_Event event)
{
  // mouse wheel is viewport by default
  enum target target = getFinalTarget(VIEWPORT);

  //////
}

// the main sdl gui loop
void mainLoop()
{
  // draw the screen for the first time
  redrawScreen();
  
  // wait for sdl events
  SDL_Event event;
  while(SDL_WaitEvent(&event))
    {
      switch(event.type)
	{
	case SDL_KEYDOWN:
	case SDL_KEYUP:
	  handleKeyboardEvent(event);
	  break;
	case SDL_MOUSEMOTION:
	  handleMouseMotionEvent(event);
	  break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	  handleMouseButtonEvent(event);
	  break;
	case SDL_MOUSEWHEEL:
	  handleMouseWheelEvent(event);
	  break;
	case SDL_WINDOWEVENT:
	  handleWindowEvent(event);
	  break;
	case SDL_QUIT:
	  return;
	}
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
  
  // it all went well
  return 0;
}

// initialize the interface state
void initInterface()
{
  // set the viewport to show the whole file
  viewport.start = 0;
  viewport.stop = audioBuffer.length;

  // clear the selection
  selection.start = 0;
  selection.stop = 0;

  // set the audio cursor at the beginning
  playPosition = 0;
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

  // init the interface state
  initInterface();

  // enter main loop
  mainLoop();

  // cleanup sdl
  cleanupSDL();

  // we made it woohoo
  return 0;
}
