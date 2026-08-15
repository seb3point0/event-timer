#ifndef ZT_SOUND_H_
#define ZT_SOUND_H_

class ZectrixBoard;

enum class Cue {
    kChime,  // talk start and the wrap-up warning
    kAlert,  // end of talk
};

// Brings up the playback task. The codec itself is only powered on the first
// cue, so a session that never makes a sound never spins up the amplifier.
void SoundInit(ZectrixBoard* board);

// Queues a cue. Returns immediately: playback runs on its own task so the
// countdown keeps refreshing while a clip is sounding.
void SoundPlay(Cue cue, int repeats = 1);

// Cuts anything currently sounding, for when the operator ends a session.
void SoundStop();

#endif  // ZT_SOUND_H_
