# Axell core - fixes applied to the JUCE source at configure time (idempotent: each fix carries a marker).
#
# 1. VST3 hosting on Linux, RunLoop::Impl (juce_VST3PluginFormat*.{cpp,h}): the fd callback captured a raw `this`.
#    JUCE's event loop copies the callbacks of every ready fd before calling them, so when an earlier callback in the
#    same pass deleted the last plugin instance (destroying the shared Impl), a plugin's callback still ran on the
#    freed Impl -> crash in eventHandlerMap[] (seen in pluginval at exit, ~15 % of runs with Axell Forbidden Seal).
#    It also iterated the handler list while a handler's onFDIsSet could re-register its fds (iterator
#    invalidation). Fixed: a weak alive flag, find() instead of operator[], a copy of the list, each handler checked
#    before it is called. Affects every JUCE-based VST3 host on Linux (the Dojo, the audit hosts, pluginval).
function(axell_fix_juce juce_dir)
  set(marker "AXELL-FIX-RUNLOOP")
  set(old "                LinuxEventLoop::registerFdCallback (fd, [this] (int descriptor)
                {
                    for (auto* h : eventHandlerMap[descriptor])
                        h->onFDIsSet (descriptor);

                    return true;
                });")
  set(new "                LinuxEventLoop::registerFdCallback (fd, [this, alive = std::weak_ptr<bool> (aliveFlag)] (int descriptor)
                {
                    // ${marker}: this callback may still run in the same dispatch pass after the Impl is gone (the
                    // last plugin deleted by an earlier callback), and a handler may change the map while it runs
                    if (alive.expired())
                        return true;

                    const auto found = eventHandlerMap.find (descriptor);

                    if (found == eventHandlerMap.end())
                        return true;

                    const auto handlersNow = found->second;

                    for (auto* h : handlersNow)
                    {
                        if (alive.expired())
                            break;

                        const auto now = eventHandlerMap.find (descriptor);

                        if (now == eventHandlerMap.end() || std::find (now->second.begin(), now->second.end(), h) == now->second.end())
                            continue;

                        h->onFDIsSet (descriptor);
                    }

                    return true;
                });")
  set(oldMember "        std::unordered_map<Linux::FileDescriptor, std::vector<Linux::IEventHandler*>> eventHandlerMap;")
  set(newMember "        std::unordered_map<Linux::FileDescriptor, std::vector<Linux::IEventHandler*>> eventHandlerMap;
        std::shared_ptr<bool> aliveFlag = std::make_shared<bool> (true);   // ${marker}")
  foreach(rel modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h
              modules/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp)
    set(f "${juce_dir}/${rel}")
    if(NOT EXISTS "${f}")
      continue()
    endif()
    file(READ "${f}" text)
    string(FIND "${text}" "${marker}" done)
    if(NOT done EQUAL -1)
      message(STATUS "Axell: JUCE run-loop fix already in ${rel}")
      continue()
    endif()
    string(FIND "${text}" "${old}" at)
    if(at EQUAL -1)
      continue()   # this file does not hold the run loop (or JUCE changed it: then the fix is not needed or must be redone)
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    string(REPLACE "${oldMember}" "${newMember}" text "${text}")
    file(WRITE "${f}" "${text}")
    message(STATUS "Axell: JUCE run-loop fix applied to ${rel}")
  endforeach()
endfunction()
