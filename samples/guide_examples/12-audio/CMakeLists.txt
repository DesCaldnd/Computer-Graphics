# Examples for docs/guide/12-audio.md (run headless: AudioEngine in offline mode, no device needed).
ox_guide_example(audio SOURCES basics.cpp spatial.cpp mixer.cpp DEPS Oxwald::audio)
ox_guide_example(audio_occlusion SOURCES occlusion.cpp DEPS Oxwald::audio Oxwald::physics)
