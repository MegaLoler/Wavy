/* TODO:
 *   variable audio format (not hardcoded to 16bit mono)
 *   dynamic allocate audio buffer rather than set max
 *   fix crash on resize while playing
 *   rendering optimizations:
 *     hardware accelerate the waveform rendering
 *     only rerender damaged areas
 *     drop redundant ui events? like mouse motion?
 *     vsync the playback animation
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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
#define SCROLL_PAN_SCALE 8
#define SCROLL_ZOOM_SCALE 0.1
#define KEY_STEP_SCALE 10
#define KEY_PAN_SCALE 30
#define KEY_ZOOM_SCALE 0.15
#define PLAY_BUFFER_SIZE 1024
#define ASYNC_PLAY_ANIMATION 0
#define EXPORT_FILE_NAME "~/tmp.mp3"

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
SDL_AudioDeviceID audioDevice; // sdl audio device id

// user input related state
int playPosition; // current sample
struct region selection; // currently selected portion
struct region viewport; // currently viewed portion of audio
enum action selectionGrabbedPole; // currently grabbed end
int looping; // currently looping or not
int playing; // currently playing or not

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
  int16_t* buffer;
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
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
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
  SDL_CloseAudioDevice(audioDevice);
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
double sumOfSquares(int offset, int length, int16_t* array, int arrayLength)
{
  int i;
  double sum = 0;
  for(i = offset; i < offset + length; i++)
    {
      double value = ((i < 0) || (i >= arrayLength)) ? 0 : array[i];
      sum += value * value;
    }
  return sum;
}

// calculate the root mean square of a range of values in an array
double rootMeanSquare(int offset, int length, int16_t* array, int arrayLength)
{
  return sqrt(sumOfSquares(offset, length, array, arrayLength) / length);
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
  int samplePeak = INT16_MAX;

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
	  float samplePercent = rootMeanSquare(sampleIndex, minSamplesPerPixel, buffer.buffer, buffer.length) / samplePeak;
	  int filledHeight = height * samplePercent;
	  int unfilledHeight = height - filledHeight;
      
	  // get rects of this column
	  SDL_Rect filledRect = { i, unfilledHeight / 2, 1, filledHeight };
	  SDL_Rect unfilledRect = { i, 0, 1, unfilledHeight / 2 };
	  SDL_Rect unfilledRect2 = { i, filledHeight + unfilledHeight / 2, 1, height - (filledHeight + unfilledHeight / 2) };

	  // get the fill colors
	  Uint32 filledColor;
	  Uint32 unfilledColor;
	  // the appropriate waveform color depends on whether its in the user selected region or not
	  if(inSelection(sampleIndex))
	    {
	      filledColor = SDL_MapRGB(surface->format, 255, 255, 0);
	      unfilledColor = SDL_MapRGB(surface->format, 63, 63, 0);
	    }
	  else
	    {
	      filledColor = SDL_MapRGB(surface->format, 255, 0, 0);
	      unfilledColor = SDL_MapRGB(surface->format, 0, 0, 0);
	    }

	  // fill this column
	  SDL_FillRect(surface, &filledRect, filledColor);
	  SDL_FillRect(surface, &unfilledRect, unfilledColor);
	  SDL_FillRect(surface, &unfilledRect2, unfilledColor);
	}
    }
}

// update the window title to show status
void updateWindowTitle()
{
  char title[32];
  sprintf(title, "Wavy: [%s] [%s]",
	  playing ? "P" : "-",
	  looping ? "L" : "-");
  SDL_SetWindowTitle(mainWindow, title);
}

// used to draw the screen when something changes
// todo: only update within a damaged rect ?
void redrawScreen()
{
  // this is all just temp stuff
  drawWaveform(mainSurface, audioBuffer, viewport);
  SDL_UpdateWindowSurface(mainWindow);
}

// play if paused, pause if playing
void togglePlaying()
{
  playing = !playing;
  updateWindowTitle();
  // unpause if playing
  if(playing) SDL_PauseAudioDevice(audioDevice, 0);
}

// toggle whether audio should loop
void toggleLooping()
{
  looping = !looping;
  updateWindowTitle();
}

// sdl audio fetch callback for more audio
void requestAudio(void* userdata, Uint8* stream, int remainingBytes)
{
  if(playing)
    {
      // if playing, fill the provided buffer with audio to play
      // copy regions of audio until the end of file or region
      // then either stop or loop depending on looping status
      int offset = 0;
      while(remainingBytes > 0)
	{
	  int remainingSamples = remainingBytes / sizeof(int16_t);
	  // get the nearest stopping point
	  int end, start;
	  if(selectionExists())
	    {
	      end = max(selection.start,
			selection.stop);
	      start = min(selection.start,
			  selection.stop);
	    }
	  else
	    {
	      end = audioBuffer.length;
	      start = 0;
	    }
	  // make sure the play position isnt greater than the end
	  // or less than start
	  if(playPosition > end) playPosition = end;
	  if(playPosition < start ||
	     playPosition == end)
	    playPosition = start;
	  int distance = end - playPosition;

	  // only copy to the nearest stopping point
	  int len;
	  if(distance < remainingSamples)
	    len = distance;
	  else
	    len = remainingSamples;
	  int lenBytes = len * sizeof(int16_t);

	  // copy this portion
	  memcpy(stream + offset,
		 audioBuffer.buffer + playPosition,
		 lenBytes);
	  playPosition += len;
	  offset += lenBytes;
	  remainingBytes -= lenBytes;

	  // if theres some left over, see if loop is on
	  if(remainingBytes > 0)
	    {
	      if(looping)
		{
		  // loop back to begining of selection
		  playPosition = start;
		}
	      else
		{
		  // looks like loop is off
		  // meaning playback has got to end here. >:(
		  togglePlaying();
		  // be sure to update the screen
		  // (not reduntant, below is only when playing)
		  // but this is a temp workaround
		  // to /help/ prevent crashing when playing
		  // while resizing the window at the same time
		  redrawScreen();
		  // and fill the rest with silecnc while ur at it
		  memset(stream + offset, 0, remainingBytes);
		  break; // <- very very important!! ><
		}
	    }
	}
      
      // redraw the screen for the playhead
      if(!ASYNC_PLAY_ANIMATION && playing) redrawScreen();
    }
  else
    {
      // if not playing, got to give sdl some silence
      //memset(stream, 0, remaining);
      // also as a bonus pause audio device if not playing
      SDL_PauseAudioDevice(audioDevice, !playing);
    }
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

// get either primary or secondary value of target
int getTargetValue(enum target target, enum action which)
{
  // get whichever one it is
  switch(which)
    {
    case PRIMARY:
      return getTargetValues(target).primary;
    case SECONDARY:
      return getTargetValues(target).secondary;
    }
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

// set both values but separate
void setTargetPrimaryAndSecondaryValues(enum target target, int primary, int secondary)
{
  struct targetValues new = { primary, secondary };
  setTargetValues(target, new);
}

// get the sample index at a pixel position
int pixelCoordinateToSample(int x)
{
  // get pixel coordinates
  int width = mainSurface->w;

  // viewport stuff
  int viewportStartSample = viewport.start;
  int viewportEndSample = viewport.stop;
  int sampleRange = viewportEndSample - viewportStartSample;
  float samplesPerPixel = 1.0 * sampleRange / width;
  int sampleIndex = viewportStartSample + x * samplesPerPixel;

  // return the sample index
  return sampleIndex;
}

// get the pixel position of a sample
int sampleToPixelCoordinate(int position)
{
  // get pixel coordinates
  int width = mainSurface->w;

  // viewport stuff
  int viewportStartSample = viewport.start;
  int viewportEndSample = viewport.stop;
  int sampleRange = viewportEndSample - viewportStartSample;
  float samplesPerPixel = 1.0 * sampleRange / width;
  int pixelPosition = (position - viewportStartSample) / samplesPerPixel;

  // return the pixel position
  return pixelPosition;
}

// get the sample index at the mouse position
int getMouseSamplePosition(SDL_Event event)
{
  return pixelCoordinateToSample(event.motion.x);
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
      // grab the nearest pole
      selectionGrabbedPole = getNearestSelectionPole(position);

      // get the pixel position of that pole
      int pole = getTargetValue(REGION, selectionGrabbedPole);
      int pixelPosition = sampleToPixelCoordinate(pole);

      // warp the mouse to the pole
      SDL_WarpMouseInWindow(mainWindow, pixelPosition, mainSurface->h / 2);
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

// general zoom the viewport
void zoom(int origin, double amount)
{
  // calculate scale factor from zoom amount
  double scale = pow(2, amount);

  // get the distance the origin is from both ends of the viewport
  int startDelta = viewport.start - origin;
  int stopDelta = viewport.stop - origin;

  // scale those distances
  startDelta /= scale;
  stopDelta /= scale;

  // set the new viewport
  setTargetPrimaryAndSecondaryValues(VIEWPORT,
				     origin + startDelta,
				     origin + stopDelta);
}

// general pan the viewport
void pan(int delta)
{
  // get pixel coordinates
  int width = mainSurface->w;

  // viewport stuff
  int viewportStartSample = viewport.start;
  int viewportEndSample = viewport.stop;
  int sampleRange = viewportEndSample - viewportStartSample;
  float samplesPerPixel = 1.0 * sampleRange / width;
  int deltaSamples = delta * samplesPerPixel;

  // set the new viewport
  setTargetPrimaryAndSecondaryValues(VIEWPORT,
				     viewport.start - deltaSamples,
				     viewport.stop - deltaSamples);
}

// pan the viewport with mouse drag
void dragViewport(SDL_Event event)
{
  pan(event.motion.xrel);
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
      dragViewport(event);
      break;
    }
}

// handle window events
int handleWindowEvent(SDL_Event event)
{
  switch(event.window.event)
    {
    case SDL_WINDOWEVENT_SIZE_CHANGED:
      // temporary to (help) prevent crashing while playing...
      // still doesnt solve it tho
      if(playing) togglePlaying();
      // anyway, get the new surface
      mainSurface = SDL_GetWindowSurface(mainWindow);
      break;
    case SDL_WINDOWEVENT_EXPOSED:
      redrawScreen();
      break;
    }

  // return 0 to mean no quit
  return 0;
}

// handle key events
int handleKeyboardEvent(SDL_Event event)
{
  // ignure key ups
  if(event.key.type == SDL_KEYDOWN)
    {
      // key events are all position by default
      enum target target = getFinalTarget(PLAY);
      
      // get pixel coordinates
      int width = mainSurface->w;

      // viewport stuff
      int viewportStartSample = viewport.start;
      int viewportEndSample = viewport.stop;
      int sampleRange = viewportEndSample - viewportStartSample;
      float samplesPerPixel = 1.0 * sampleRange / width;
      int step = KEY_STEP_SCALE * samplesPerPixel;
      
      // which physical key?
      // arrow keys
      switch(target)
	{
	case PLAY:
	case REGION:
	  switch(event.key.keysym.scancode)
	    {
	    case SDL_SCANCODE_RIGHT:
	      setTargetPrimaryValue(target, getTargetValues(target).primary + step);
	      break;
	    case SDL_SCANCODE_LEFT:
	      setTargetPrimaryValue(target, getTargetValues(target).primary - step);
	      break;
	    case SDL_SCANCODE_DOWN:
	      setTargetSecondaryValue(target, getTargetValues(target).secondary - step);
	      break;
	    case SDL_SCANCODE_UP:
	      setTargetSecondaryValue(target, getTargetValues(target).secondary + step);
	      break;
	    }
	  break;
	case VIEWPORT:
	  switch(event.key.keysym.scancode)
	    {
	    case SDL_SCANCODE_UP:
	      zoom(pixelCoordinateToSample(mainSurface->w / 2),
		   KEY_ZOOM_SCALE);
	      break;
	    case SDL_SCANCODE_DOWN:
	      zoom(pixelCoordinateToSample(mainSurface->w / 2),
		   -KEY_ZOOM_SCALE);
	      break;
	    case SDL_SCANCODE_LEFT:
	      pan(KEY_PAN_SCALE);
	      break;
	    case SDL_SCANCODE_RIGHT:
	      pan(-KEY_PAN_SCALE);
	      break;
	    }
	  break;
	}
      
      // now which virtual key?
      // these are for mnemonic hotkeys
      switch(event.key.keysym.sym)
	{
	case SDLK_SPACE:
	  // play pause key
	  togglePlaying();
	  break;
	case SDLK_l:
	  // toggle looping
	  toggleLooping();
	  break;
	case SDLK_e:
	  // export selected snippet
	  exportSnippet();
	  break;
	case SDLK_ESCAPE:
	case SDLK_q:
	  // quit
	  return -1;
	  break;
	}
    }

  // return 0 to mean no quit
  return 0;
}

// handle mouse move events
int handleMouseMotionEvent(SDL_Event event)
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
  
  // return 0 for no quit event
  return 0;
}

// handle mouse button events
int handleMouseButtonEvent(SDL_Event event)
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
	  // grab mouse cursor
	  SDL_SetRelativeMouseMode(1);
	  break;
	case REGION:
	  // initiate a selection
	  initiateSelection(getMouseSamplePosition(event));
	  break;
	case VIEWPORT:
	  // grab mouse cursor
	  SDL_SetRelativeMouseMode(1);
	  break;
	}
    }
  else
    {
      // release mouse cursor
      SDL_SetRelativeMouseMode(0);
    }
  
  // return 0 for no quit event
  return 0;
}

// handle mouse wheel events
int handleMouseWheelEvent(SDL_Event event)
{
  // mouse wheel is viewport by default
  enum target target = getFinalTarget(VIEWPORT);

  // do a general pan for how much was scrolled horizontally
  pan(event.wheel.x * SCROLL_PAN_SCALE);
  
  // and a general zoom for how much was scrolled vertically
  int x;
  SDL_GetMouseState(&x, NULL);
  zoom(pixelCoordinateToSample(x), event.wheel.y * SCROLL_ZOOM_SCALE);
  
  // return 0 for no quit event
  return 0;
}

// process an SDL event
int processEvent(SDL_Event event)
{
  switch(event.type)
    {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      return handleKeyboardEvent(event);
    case SDL_MOUSEMOTION:
      return handleMouseMotionEvent(event);
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
      return handleMouseButtonEvent(event);
    case SDL_MOUSEWHEEL:
      return handleMouseWheelEvent(event);
    case SDL_WINDOWEVENT:
      return handleWindowEvent(event);
    case SDL_QUIT:
      // return non-zero to mean exit
      return -1;
    }
  // return 0 for no quit event
  return 0;
}

// the main sdl gui loop
void mainLoop()
{
  // wait for sdl events
  SDL_Event event;

  // wait for events until a quit event is received
  while(1)
    {
      // when playing, we need to animate every frame
      // if set that way anyways
      if(ASYNC_PLAY_ANIMATION && playing)
	{
	  while(SDL_PollEvent(&event))
	    {
	      if(processEvent(event)) return;
	    }
	  // update the screen for the playhead
	  redrawScreen();
	}
      // otherwise just take events as they come
      else
	{
	  SDL_WaitEvent(&event);
	  if(processEvent(event)) break;
	}
    }
}

// save a buffer to an audio file using ffmpeg
void saveAudioToFile(struct audioBuffer buffer, const char* filename)
{
  // save the raw data with ffmpeg
  // for now just force mono and 16bit
  char cmd[128];
  sprintf(cmd, "ffmpeg -y -f s16le -ar 44100 -ac 1 -i - %s", filename);
  FILE* pipe;
  pipe = popen(cmd, "w");
  fwrite(buffer.buffer, sizeof(uint16_t), buffer.length, pipe);
  pclose(pipe);
}

// load an audio file into a buffer using ffmpeg
struct audioBuffer loadAudioFromFile(const char* filename)
{
  // create a buffer to store the data
  int16_t* buffer = (int16_t*)calloc(MAX_SAMPLES, sizeof(int16_t));
  struct audioBuffer audioBuffer = { buffer, 0 };

  // load the raw data from ffmpeg
  // for now just force mono and 16bit
  FILE* pipe;
  char cmd[128];
  sprintf(cmd, "ffmpeg -hide_banner -loglevel panic -i %s -f s16le -ac 1 -", filename);
  pipe = popen(cmd, "r");
  audioBuffer.length = fread(buffer, sizeof(int16_t), MAX_SAMPLES, pipe);
  pclose(pipe);

  // return the buffer struct
  return audioBuffer;
}

// export the selected region of audio to a tmp file
void exportSnippet()
{
  // can only export a snippet if there's a selection to export
  if(selectionExists())
    {
      int end = max(selection.start, selection.stop);
      int start = min(selection.start, selection.stop);
      int16_t* exportBuffer = audioBuffer.buffer + start;
      int exportLength = end - start;
      struct audioBuffer saveBuffer = { exportBuffer, exportLength };
      saveAudioToFile(saveBuffer, EXPORT_FILE_NAME);
    }
}

// load the given audio file from the cli
// also init sdl audio stuff
int initAudio()
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

  // init sdl audio
  // copied from sdl wiki mostly
  SDL_AudioSpec want, have;

  // desired audio out format
  SDL_memset(&want, 0, sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16;
  want.channels = 1;
  want.samples = PLAY_BUFFER_SIZE;
  want.callback = requestAudio;

  // init the audio device
  audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if(audioDevice == 0)
    {
      fprintf(stderr, "Failed to open audio: %s", SDL_GetError());
      return -1;
    }
  // unpause and have it poll all it wants muahuahuahuahuah
  SDL_PauseAudioDevice(audioDevice, !cliArgs.autoplay);
  
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

  // set things from cli args
  playing = cliArgs.autoplay;
  looping = cliArgs.autoloop;

  // update window title
  updateWindowTitle();

  // draw the screen for the first time
  redrawScreen();
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

  // load the audio file and init playback buffer
  if(initAudio())
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
