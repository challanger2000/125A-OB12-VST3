// Host-independent VST3 smoke check. No copyrighted ROM or panel assets required.
// This verifies the produced binary, factory interface, buses, state parameter guards.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
using namespace Steinberg;
using namespace Steinberg::Vst;

static int fail(const char* why) { printf("FAIL: %s\n",why);return 1; }
int main(int argc,char** argv) {
    if (argc != 2) return fail("expect module path");
    HMODULE library = LoadLibraryA(argv[1]);
    if (!library) return fail("unable to load VST3 DLL");
    auto factory_fn = (IPluginFactory* (PLUGIN_API*)()) GetProcAddress(library,"GetPluginFactory");
    if (!factory_fn) return fail("GetPluginFactory missing");
    IPluginFactory* factory=factory_fn();
    if (!factory || factory->countClasses()<1) return fail("factory has no classes");
    PClassInfo info{};
    if (factory->getClassInfo(0,&info)!=kResultOk) return fail("getClassInfo failed");
    IComponent* component=nullptr;
    if (factory->createInstance(info.cid,IComponent::iid,(void**)&component)!=kResultOk || !component)
        return fail("cannot instantiate IComponent");
    if (component->initialize(nullptr)!=kResultOk) return fail("cannot initialize component");
    if (component->getBusCount(kAudio,kOutput)!=1 || component->getBusCount(kEvent,kInput)!=1)
        return fail("incorrect audio or MIDI event buses");
    IAudioProcessor* audio=nullptr;
    if (component->queryInterface(IAudioProcessor::iid,(void**)&audio)!=kResultOk || !audio)
        return fail("IAudioProcessor unavailable");
    IEditController* editor=nullptr;
    if (component->queryInterface(IEditController::iid,(void**)&editor)!=kResultOk || !editor)
        return fail("IEditController unavailable");
    if (editor->getParameterCount()<100) return fail("MIDI mapping parameters missing");
    if (component->getState(nullptr)!=kInvalidArgument || component->setState(nullptr)!=kInvalidArgument)
        return fail("component accepted null state stream");
    if (audio->canProcessSampleSize(kSample32)!=kResultTrue) return fail("32-bit processing rejected");
    editor->release();
    audio->release();
    if (component->terminate()!=kResultOk) return fail("cannot terminate");
    component->release();
    printf("PASS: VST3 factory, COM interfaces, buses, state guards, lifecycle\n");
    return 0;
}
