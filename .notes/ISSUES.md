# Open issues

- `--turn-weather` asks the sky for the next weather every four seconds of world (`RtxTool::Session::turnWeather`, `sTurnFrames`), but a transition runs 1 / `Transition_Delta` simulation seconds (25–67 s) and `WeatherManager` keeps one queued request, so a bench turns one weather per transition and the halfway precipitation swap the option exists for lands only in a run longer than half a transition.
- `SDL_CONTROLLERDEVICEREMAPPED` (0x655) is not handled in `SDLUtil::InputWrapper::capture`, so a game started with a controller attached logs "Unhandled SDL event".
