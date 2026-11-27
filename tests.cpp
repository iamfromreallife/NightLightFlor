#include "core.hpp"
#include "localization.hpp"
#include "location_search_state.hpp"
#include "solar_time.hpp"
#include "map_view.hpp"
#include "display_identity.hpp"
#include "ddc.hpp"
#include "motion.hpp"
#include "update_version.hpp"
#include "release_markdown.hpp"
#include "wmi_brightness.hpp"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <chrono>
#include <limits>
using namespace nightlight;
using portable::Json;
namespace fs=std::filesystem;
void check(bool ok,const char* message) {
    if(!ok) throw std::runtime_error(message);
}
int main() {
    try {
        {
            LocationSearchState q;
            q.changed(L"Li",0);check(!q.busy()&&!q.ready(1000),"Short queries do not trigger suggestions");
            q.changed(L"Lis",100);check(!q.ready(499)&&q.ready(500),"400 ms suggestion debounce");
            q.changed(L"Lisbon",450);check(!q.ready(500)&&q.ready(850),"Typing restarts debounce");
            q.started();q.changed(L"London",900);check(!q.ready(2000),"At most one search in flight");
            check(!q.finished()&&q.ready(2000),"Stale city search discarded; latest query queued");
            q.started();q.manual(2001);check(q.explicitSelection&&!q.ready(2001),"Enter reuses running search");
            check(q.finished()&&q.completed==L"London"&&!q.busy(),"Current search accepted without duplicate request");
            q.changed(L"Paris",3000);q.started();q.changed(L"",3500);
            check(!q.finished()&&!q.busy(),"Clearing search invalidates old results");
            q.changed(L"LA",4000);q.manual(4000);check(q.ready(4000),"Explicit search allows two characters");
        }
        {
            // Serialization must remain byte-identical to the old stream
            // representation, even at signed limits and for negative zero.
            for(double value : {-2147483648.0,-1000000.0,-1.0,-0.0,0.0,1.0,6500.0,2147483647.0,2147483648.0,1.25,1e20,1e-8}) {
                std::ostringstream expected;expected.imbue(std::locale::classic());expected<<std::setprecision(15)<<value;
                check(Json(value).dump()==expected.str(),"Fast JSON integer output matches legacy serialization");
            }
            const std::string nested=R"({"a":[1,{"b":"text"},true,null],"z":-5})";
            check(Json::parse(nested).dump()==nested,"Move-based JSON parser preserves nested values");
            check(Json::parse(R"({"a":1,"a":2})").dump()==R"({"a":2})","Duplicate JSON key retains last value");
        }
        {
            check(uiLanguages.size()==10,"All ten UI languages are available");
            check(std::wstring_view(languageAt(1).name)==L"Português" &&
                  std::wstring_view(languageAt(1).code)==L"pt-PT","Short Portuguese label retains the PT-PT locale");
            for(size_t i=0;i<uiLanguages.size();++i)check(languageIndex(uiLanguages[i].code)==static_cast<int>(i),"Locale ID round trip");
            check(languageIndex(L"pt")==1 && languageIndex(L"invalid")==0,"Legacy and unknown locale mapping");
            for(const auto& message:uiMessages)for(int language=1;language<10;++language){
                check(std::wstring_view(translate(language,message.key.data()))==message.texts[language-1],"Complete translated string lookup");
                check(message.texts[language-1][0]!=0,"Non-empty translation");
            }
        }
        {
            ReleaseMarkdown md;
            const auto rtf=md.render(L"## Release\n- **Fixed** *italic* ~~old~~ `code`\n- ***Both***\n- `#ff0000`\n- <span style=\"color: #12ab34;\">colour</span>\n- [GitHub](https://github.com/x)\n- {literal} \\object \u00e7\n```cpp\n**not bold**\n```",0xf5f5f5,0xee4b23,21);
            check(rtf.find("{\\b Fixed}")!=std::string::npos && rtf.find("{\\i italic}")!=std::string::npos &&
                  rtf.find("{\\strike old}")!=std::string::npos && rtf.find("{\\b\\i Both}")!=std::string::npos,
                  "Release Markdown emphasis and strikethrough");
            check(rtf.find("\\red255\\green0\\blue0;")!=std::string::npos && rtf.find("\\red18\\green171\\blue52;")!=std::string::npos,
                  "Release Markdown colour swatches and inline colour");
            check(rtf.find("{\\f1 **not bold**}")!=std::string::npos && rtf.find("\\{literal\\}")!=std::string::npos &&
                  rtf.find("\\\\object")!=std::string::npos && rtf.find("\\u231?")!=std::string::npos,
                  "Release Markdown code and RTF escaping");
        }
        {
            check(parseScheduleTime(L"7pm")==1140 && parseScheduleTime(L"7")==420 &&
                  parseScheduleTime(L"7h00")==420 && parseScheduleTime(L"07h00")==420 &&
                  parseScheduleTime(L"07h")==420 && parseScheduleTime(L"7.30")==450 &&
                  parseScheduleTime(L"12am")==0 && parseScheduleTime(L"12pm")==720,
                  "Common time-entry formats and noon/midnight");
            check(!parseScheduleTime(L"25:00")&&!parseScheduleTime(L"7h72")&&!parseScheduleTime(L"7abc")&&
                  !parseScheduleTime(L"13pm")&&!parseScheduleTime(L"7:30junk"),"Reject invalid time entries");
            const auto eq=solarUtcMinutes(80,365,0,0);
            check(eq[0]&&eq[1]&&eq[2]&&std::abs(*eq[0]-360)<20&&std::abs(*eq[2]-1080)<20&&
                  *eq[0]<*eq[1]&&*eq[1]<*eq[2],"Equatorial sunrise, solar noon and sunset");
            const auto polar=solarUtcMinutes(172,365,89,0);
            check(!polar[0]&&polar[1]&&!polar[2],"Polar day has no fabricated rise or set event");
            check(dayOfYear(2024,3,1)==61&&dayOfYear(2026,3,1)==60,"Leap-year solar date");
            auto configured=ColorSchedule{};configured.points[2].customTime=true;
            const auto daily=effectiveSolarPoints(configured,{410,730,1180});
            check(daily.points[0].minute==410 && daily.points[1].minute==730 && daily.points[2].minute==1140,"Daily solar refresh respects manual overrides");
            configured.points.push_back({410,2000,SchedulePointKind::Custom,true});
            const auto coincident=effectiveSolarPoints(configured,{410,730,1180});
            check(coincident.points.front().kelvin==2000 && configured.points.size()==4,"Custom point wins temporary solar overlap without losing configured points");
            const auto city=projectMapPoint(37.74,-25.67,10),zoomed=projectMapPoint(37.74,-25.67,11);
            check(std::abs(zoomed.x-2*city.x)<0.001 && std::abs(zoomed.y-2*city.y)<0.001,"Map zoom retains geographic centre");
            const auto tiles=visibleMapTiles(10,city,452,424);
            check(!tiles.empty()&&tiles.size()<=9,"Only visible map tiles requested");
            check(wrappedMapDelta(1023,1024)==-1 && wrappedMapDelta(-1023,1024)==1,"Map wraps across the antimeridian");
            HotkeyBindings steps;steps[L""]={10,15,250,500};steps[L"MONITOR-A"]={2,3,50,80};
            check(hotkeyStepsFor(steps,L"")[3]==500&&hotkeyStepsFor(steps,L"MONITOR-A")[0]==2&&
                  hotkeyStepsFor(steps,L"UNKNOWN")==kDefaultHotkeySteps&&
                  !validHotkeyStep(0,101)&&!validHotkeyStep(2,0),"Independent configurable hotkey increments");
        }
        {
            ColorSchedule schedule{true,{{420,6500},{720,1000},{1140,3500}},ScheduleMode::Automatic,30};
            check(scheduledColor(schedule,420*60).kelvin==6500 &&
                  scheduledColor(schedule,570*60).kelvin==3750 &&
                  scheduledColor(schedule,720*60).kelvin==1000 &&
                  scheduledColor(schedule,1140*60).kelvin==3500 &&
                  scheduledColor(schedule,60*60).kelvin==5000,"Automatic interpolation reaches every point, including midnight");
            schedule.mode=ScheduleMode::Custom;
            check(scheduledColor(schedule,689*60).kelvin==6500 &&
                  scheduledColor(schedule,705*60).kelvin==3750 && scheduledColor(schedule,720*60).kelvin==1000,
                  "Custom transition reaches target at selected time");
            schedule.mode=ScheduleMode::Instant;
            check(scheduledColor(schedule,720*60-1).kelvin==6500 && scheduledColor(schedule,720*60).kelvin==1000,
                  "Instant switches at the point");
            check(!uniqueScheduleTimes({{1,1000},{1,6500}}) && uniqueScheduleTimes(schedule.points),"Reject duplicate times");
            auto normalized=normalizedSchedule({true,{{1439,900},{1,7000}},ScheduleMode::Automatic,200});
            check(normalized.points.front().minute==1 && normalized.points.front().kelvin==6500 &&
                  normalized.points.back().kelvin==1000 && normalized.transitionMinutes==120,"Normalize and sort points");
            check(validHotkey(0) && validHotkey(0x0241) && !validHotkey(0x41) && !validHotkey(-1),
                  "Hotkeys must use a modifier and a valid virtual key");
        }
        {
            check(newerThan("v1.0.20","1.0.19") && newerThan("2.0.0","1.0.19") && !newerThan("v1.0.19","1.0.19") && !newerThan("v1.0.4","1.0.19"),"Numeric release comparison");
            check(parseReleaseVersion("v2.0")==parseReleaseVersion("2.0.0") && newerThan("v2.0","1.0.41") && !newerThan("v2.0.0","2.0") && newerThan("2.1","2.0"),"Two-component release versions and zero patch equivalence");
            check(!newerRelease(kAppVersion) && parseReleaseVersion(kAppVersion).has_value(),"Installed build version is valid and not offered as an update");
            check(!parseReleaseVersion("v1.0.20-beta") && !parseReleaseVersion("../2.0.0") && !parseReleaseVersion("1") && !parseReleaseVersion("2.0."),"Reject prerelease and unsafe tag syntax");
        }
        {
            check(dimOverlayAlpha(0)==0 && dimOverlayAlpha(50)==128 &&
                  dimOverlayAlpha(90)==230 && dimOverlayAlpha(99)==252 &&
                  dimOverlayAlpha(100)==252,"Black overlay opacity is bounded and never completely opaque");
            check(!displayIdleLongEnough(119999,0,2) && displayIdleLongEnough(120000,0,2) &&
                  !displayIdleLongEnough(1,100,2),"Timeout boundaries and monotonic activity");
            check(sessionInputAge(0x20u,0xfffffff0u)==48 &&
                  sessionInputAge(1000u,900u)==100 &&
                  sessionInputAge(1000u,1001u)==0,
                  "Windows session input clock handles 32-bit rollover and backwards samples");
            check(dimFadeDuration(0,230)==2000 && dimFadeDuration(230,0)==200 &&
                  dimFadeAlpha(0,230,0)==0 && dimFadeAlpha(0,230,1000)==115 &&
                  dimFadeAlpha(0,230,2000)==230 && dimFadeAlpha(230,0,100)==115 &&
                  dimFadeAlpha(230,0,200)==0 && dimFadeAlpha(230,0,999)==0,
                  "Black overlay fades in over 2 seconds and out over 200 ms");
            check(kStartupColorMs==1000 && startupColorKelvin(2500,0)==6500 && startupColorKelvin(2500,500)==4500 &&
                  startupColorKelvin(2500,1000)==2500 && startupColorKelvin(2500,9000)==2500 &&
                  startupColorKelvin(6500,500)==6500,
                  "Launch color transition begins neutral and reaches the exact saved target");
            int previous=6500;
            for(unsigned ms=0;ms<=1000;++ms) {
                const int next=startupColorKelvin(1000,ms);
                check(next<=previous && next>=1000,"Startup color warms monotonically without overshoot");
                previous=next;
            }
            // A stationary pointer on a single monitor does not continually
            // renew last input. The system idle time reaches the timeout.
            const auto now=120000ull;
            const auto last=now-sessionInputAge(120000u,0u);
            check(displayIdleLongEnough(now,last,2),
                "A parked mouse does not prevent screen-saver-style inactivity");
            Monitor fallback;fallback.device=L"DISPLAY-FALLBACK";
            Preferences dim;
            check(dim.dims(fallback),"All-display overlay does not require hardware identity");
            dim.dimAll=false;dim.dimDisplays.insert(monitorProfileKey(fallback));
            check(dim.dims(fallback),"Individual display can be selected using its live device fallback");
            check(!confirmedForegroundMedia(false,100,200) &&
                  !confirmedForegroundMedia(true,0,200) &&
                  !confirmedForegroundMedia(true,100,8101) &&
                  confirmedForegroundMedia(true,100,8100),
                  "Missing, failed or stale media checks cannot indefinitely prevent dimming");
            check(videoMayBePlaying(true,ReportedMediaKind::Video) &&
                  videoMayBePlaying(true,ReportedMediaKind::Unknown) &&
                  !videoMayBePlaying(false,ReportedMediaKind::Video) &&
                  !videoMayBePlaying(true,ReportedMediaKind::Music) &&
                  !videoMayBePlaying(true,ReportedMediaKind::Image),"Only playing video or unknown media globally inhibits dimming");
            check(mediaExemptsDisplay(true,false,false) && mediaExemptsDisplay(false,true,true) &&
                  !mediaExemptsDisplay(false,true,false) && !mediaExemptsDisplay(false,false,true),
                  "Video protection is global; foreground audio remains per display");
        }
        {
            DisplayTargetSnapshot snapshot;
            snapshot.complete=true;
            DisplayTarget target;
            target.physicalId=L"PHYSICAL-A";
            target.targetId=7;
            target.adapterId.LowPart=42;
            snapshot.bySource[L"DISPLAY-A"].push_back(target);
            const auto* matched=matchPhysicalTarget(snapshot,L"display-a",L"physical-a");
            check(matched && matched->targetId==7 && matched->adapterId.LowPart==42,
                "Shared identity/SDR snapshot retains exact adapter and target");
            check(!matchPhysicalTarget(snapshot,L"display-a",L"physical-b"),
                "Renumbered or mismatched physical target is rejected");
            snapshot.complete=false;
            check(!matchPhysicalTarget(snapshot,L"display-a",L"physical-a"),
                "Incomplete topology cannot authorize hardware writes");
            snapshot.complete=true;
            snapshot.bySource[L"DISPLAY-A"].push_back(target);
            check(!matchPhysicalTarget(snapshot,L"display-a",L"physical-a"),
                "Ambiguous or mirrored target cannot authorize hardware writes");
        }
        {
            Monitor mon;
            check(monitorNeedsColorWatchdog(mon),"GDI retains its periodic watchdog");
            mon.nvidiaTargetColor=true;
            check(!monitorNeedsColorWatchdog(mon),"Pending NVIDIA setter does not poll");
            mon.gammaStatus=1;
            check(!monitorNeedsColorWatchdog(mon),"Accepted NVIDIA setter has no polling work");
            mon.gammaStatus=0;
            check(monitorNeedsColorWatchdog(mon),"Failed NVIDIA setter remains retryable");
        }
        {
            const auto neutral=buildNvidiaTargetRamp(6500);
            const auto warm=buildNvidiaTargetRamp(4000);
            check(neutral.size()==3072 && neutral[0]==0 && neutral[3069]==1 &&
                neutral[3070]==1 && neutral[3071]==1,"Target gamma identity endpoints");
            check(warm[3069]==1 && warm[3070]<1 && warm[3071]<warm[3070],"Target gamma warm RGB order");
            for(size_t i=1;i<1024;++i)
                for(size_t c=0;c<3;++c)
                    check(warm[3*i+c]>=warm[3*(i-1)+c] && warm[3*i+c]<=1,
                          "Target gamma channel monotonicity");
        }
        check(hardwareGainTarget(0,80,1.0)==80,"Hardware neutral retains calibration");
        check(hardwareGainTarget(10,90,0.5)==50,"Hardware gain uses baseline and minimum");
        check(hardwareGainTarget(10,90,0.0)==10,"Hardware gain clamps to minimum");
        check(sliderPositionFromX(-1000,10,210,0,10000)==0,"Captured dragging clamps beyond the left edge");
        check(sliderPositionFromX(1000,10,210,0,10000)==10000,"Captured dragging clamps beyond the right edge");
        check(sliderPositionFromX(110,10,210,0,10000)==5000,"Captured dragging maps the pointer continuously");
        check(sliderPositionFromX(110,10,10,0,10000)==0,"Degenerate slider geometry stays safe");
        for(int width:{1,16,75,250,1000})for(int inset:{5,10,20}){
            const auto track=ddcTrackBounds(width,inset);
            check(track.right>track.left,"DDC rail has a positive span");
            for(int x=track.left;x<=track.right;++x){
                const auto position=sliderPositionFromX(x,track.left,track.right,0,10000);
                check(sliderXFromPosition(position,track.left,track.right,0,10000)==x,
                      "DDC thumb uses the same geometry as captured input");
            }
            check(sliderXFromPosition(0,track.left,track.right,0,10000)==track.left &&
                  sliderXFromPosition(10000,track.left,track.right,0,10000)==track.right &&
                  sliderXFromPosition(2500,track.left,track.right,0,10000)<=sliderXFromPosition(7500,track.left,track.right,0,10000),
                  "DDC thumb coordinates remain monotonic with no native thumb rectangle");
        }
        // The visual brightness and contrast icons must share the exact
        // middle Y of their native trackbars, at any screen position/DPI.
        const POINT brightIcon=ddcSliderIconCenter(52,134,28);
        const POINT contrastIcon=ddcSliderIconCenter(52,164,28);
        check(brightIcon.x==52 && brightIcon.y==148,
            "Brightness Fluent glyph is centred on its 28px trackbar");
        check(contrastIcon.x==52 && contrastIcon.y==178,
            "Contrast Fluent glyph is centred on its 28px trackbar");
        const POINT scaledIcon=ddcSliderIconCenter(78,200,42);
        check(scaledIcon.x==78 && scaledIcon.y==221,
            "Fluent glyph alignment uses slider centre at 150 percent DPI");
        check(kelvinFromTemperatureSliderX(0,20,620)==1000 &&
              kelvinFromTemperatureSliderX(1000,20,620)==6500,
            "Temperature slider clamps to the original 1000-6500 K endpoints");
        check(kelvinFromTemperatureSliderX(20,20,620)==1000 &&
              kelvinFromTemperatureSliderX(620,20,620)==6500,
            "Temperature range remains unchanged at its visible endpoints");
        check(kelvinFromTemperatureSliderX(320,20,620)<3750 &&
              kelvinFromTemperatureSliderX(320,20,620)>3100,
            "Middle of the slider now produces mildly warmer Kelvin than linear");
        check(temperatureSliderXFromKelvin(1000,20,620)==20 &&
              temperatureSliderXFromKelvin(6500,20,620)==620,
            "Warm-biased slider thumb keeps both exact endpoint positions");
        int priorKelvin=1000;
        for(int x=20;x<=620;x+=10) {
            const int temperature=kelvinFromTemperatureSliderX(x,20,620);
            check(temperature>=priorKelvin && temperature%10==0,
                "Warmer slider response remains monotonic and moves in 10K steps");
            priorKelvin=temperature;
        }
        for(int k=1000;k<=6500;k+=250) {
            const int x=temperatureSliderXFromKelvin(k,20,620);
            const int restored=kelvinFromTemperatureSliderX(x,20,620);
            check(std::abs(restored-k)<=20,
                "Kelvin-to-slider inverse holds across the entire range");
        }
        check(kelvinFromTemperatureSliderX(110,110,110)==1000 &&
              temperatureSliderXFromKelvin(4000,110,110)==110,
            "Warm slider mapping handles zero-width geometry safely");
        {
            using Key=std::pair<std::wstring,int>;
            std::map<Key,int> pending{{{L"A",0},1},{{L"A",1},2},{{L"B",0},3},{{L"B",1},4}};
            Key previous{};bool first=true;std::vector<int> served;
            for (int i=0;i<4;++i) {
                auto next=nextMonitorJob(pending,previous,first);
                previous=next->first;first=false;served.push_back(next->second);pending.erase(next);
                pending[{L"A",0}]=1; // Simulate continuous high-rate dragging.
            }
            check(served==std::vector<int>({1,2,3,4}),"A hot slider cannot starve another monitor or feature");
        }
        bool tooDeep=false;
        try {
 Json::parse(std::string(140,'[')+"0"+std::string(140,']')); } catch(const std::exception&) { tooDeep=true; }
        check(tooDeep,"Excessive JSON nesting is rejected before stack exhaustion");
        {
            Motion motion(0);
            motion.to(1,100,200,true);
            const double midway=motion.value(200);
            check(midway>0 && midway<1,"Animation stays within its endpoints");
            motion.to(0,200,150,true);
            check(std::abs(motion.value(200)-midway)<0.000001,"Interrupted animation must not jump");
            motion.to(0,240,150,true);
            check(motion.value(350)==0 && !motion.active(350),"Repeated paints cannot restart an idle animation timer");
            motion.to(1,400,200,true);
            motion.to(1,450,200,false);
            check(motion.value(450)==1 && !motion.active(450),"Reduced motion snaps to the final state immediately");
        }
        {
            check(displayInsertionOrder(4,0,3)==std::vector<std::size_t>({1,2,3,0}),"Display drag forward preserves identities");
            check(displayInsertionOrder(4,3,1)==std::vector<std::size_t>({0,3,1,2}),"Display drag backward preserves identities");
            check(displayInsertionOrder(3,0,0)==std::vector<std::size_t>({0,1,2}),"Display drag returning to original slot");
            const std::vector<int> widths{210,300,150};
            const auto before=settingsDisplaySlots(widths,displayInsertionOrder(3,0,0));
            const auto after=settingsDisplaySlots(widths,displayInsertionOrder(3,0,2));
            check(before[0].x==26 && before[1].y==124 && before[2].x==332,"Display wrapping before drag");
            check(after[1].x==26 && after[2].x==332 && after[0].y==124,"Display wrapping after drag");
            Motion position(before[0].y);position.to(after[0].y,100,kDisplayReorderMs,true);
            check(position.active(100) && position.value(190)>86 && position.value(280)==124 && !position.active(280),"Settings display animation uses shared 180 ms duration");
        }
        {
            Motion spin(0);
            startRefreshSpin(spin,100,true);
            check(spin.active(100) && spin.value(100)==0 && spin.value(560)==360 && !spin.active(560),"Refresh spin is exactly 460 ms and one revolution");
            startRefreshSpin(spin,600,true);
            const auto midway=spin.value(700);
            startRefreshSpin(spin,700,true);
            check(std::abs(spin.value(700)-midway)<0.000001 && std::fmod(spin.value(1160),360.0)==0,"Repeated refresh preserves angle and returns to neutral");
            startRefreshSpin(spin,1200,false);
            check(!spin.active(1200) && std::fmod(spin.value(1200),360.0)==0,"Refresh respects reduced motion");
        }
        DWORD settingWritten=0; int directCalls=0,vcpCalls=0;
        check(writeDdcValue(false,0,0,37,[&](DWORD value){settingWritten=value;++directCalls;return true;},
            [&](DWORD){++vcpCalls;return true;}) && settingWritten==37 && directCalls==1 && vcpCalls==0,
            "Upstream compatibility: failed getters must not block a direct hardware write");
        check(writeDdcValue(true,20,220,50,[](DWORD){return false;},[&](DWORD value){settingWritten=value;return true;}) && settingWritten==120,
            "DDC writes must retain VCP fallback and honor a known hardware range");
        check(!writeDdcValue(false,0,0,45,[](DWORD){return false;},[](DWORD){return false;}),
            "DDC must report failed writes rather than claim success");
        check(kDdcReadbackIntervalMs==3000 &&
              kDdcPostWriteGraceMs>=1000 && kDdcPostWriteGraceMs<=3000,
            "Read actual monitor levels every three seconds, with settling grace after writes");
        {
            DdcValue saved;
            saved.percent=77; saved.hasValue=true; saved.managed=true;
            saved.generation=9; saved.protectUntilTickMs=1234;
            DdcValue hardware;
            hardware.supported=true; hardware.readable=true;
            hardware.hasValue=true; hardware.percent=26;
            hardware.backend=DdcBackend::DdcCi;
            applyDdcObservation(saved,hardware,false);
            check(saved.percent==26 && saved.hasValue && saved.managed &&
                  saved.readable && saved.supported &&
                  saved.backend==DdcBackend::DdcCi,
                "A valid physical brightness/contrast reading overrides old saved values");
            check(saved.generation==9 && saved.protectUntilTickMs==1234,
                "Hardware reads never alter local write ordering or the settling deadline");
            hardware.percent=68;
            applyDdcObservation(saved,hardware,false);
            check(saved.percent==68,
                "An external monitor OSD adjustment updates the live slider");
            saved.pending=true; saved.percent=91; saved.generation=10;
            hardware.percent=15;
            applyDdcObservation(saved,hardware,true);
            check(saved.percent==91 && saved.pending && saved.generation==10,
                "A queued or active user adjustment is never overwritten by stale DDC data");
            saved.pending=false;
            applyDdcObservation(saved,hardware,true);
            check(saved.percent==91,
                "A display still settling after a local write does not move the slider back");
            applyDdcObservation(saved,hardware,false);
            check(saved.percent==15,
                "Hardware readback resumes after the local write grace period");
            hardware.readable=false; hardware.hasValue=false;
            hardware.supported=true;
            applyDdcObservation(saved,hardware,false);
            check(saved.percent==15 && saved.hasValue && saved.managed &&
                  saved.supported && !saved.readable,
                "Unreadable DDC hardware keeps the last known valid value as fallback");
            hardware.supported=false;
            applyDdcObservation(saved,hardware,false);
            check(saved.percent==15 && !saved.supported,
                "Unsupported monitors are not assigned fictional readings");
            hardware.supported=true; hardware.readable=true;
            hardware.hasValue=true; hardware.percent=120;
            applyDdcObservation(saved,hardware,false);
            check(saved.percent==100,
                "Out-of-range monitor percentages are safely clamped");
        }
        {
            DdcValue target;target.percent=25;target.hasValue=true;target.managed=true;
            target.awaitingWriteReadback=true;
            DdcValue reported;reported.supported=true;reported.readable=true;reported.hasValue=true;reported.percent=100;
            applyDdcObservation(target,reported,true);
            check(target.percent==25 && target.differingReadback==-1,"Grace period ignores stale readback");
            applyDdcObservation(target,reported,false);
            check(target.percent==25 && target.awaitingWriteReadback,"First post-write mismatch cannot jump to full brightness");
            reported.percent=25;applyDdcObservation(target,reported,false);
            check(target.percent==25 && !target.awaitingWriteReadback,"Matching hardware readback confirms target");
            reported.percent=60;applyDdcObservation(target,reported,false);
            check(target.percent==60,"External changes still update confirmed sliders");
            target.percent=25;target.awaitingWriteReadback=true;reported.percent=100;
            applyDdcObservation(target,reported,false);applyDdcObservation(target,reported,false);
            check(target.percent==100 && !target.awaitingWriteReadback,"Repeated real mismatch is eventually reflected");
            target.percent=25;target.awaitingWriteReadback=true;reported.percent=100;
            applyDdcObservation(target,reported,false);reported.readable=false;
            applyDdcObservation(target,reported,false);reported.readable=true;
            applyDdcObservation(target,reported,false);
            check(target.percent==25,"Unreadable sample breaks mismatch confirmation");
        }
        check(validDdcReading(0,100,100) && validDdcReading(20,220,120) &&
              !validDdcReading(0,100,65535) && !validDdcReading(20,220,0) && !validDdcReading(50,50,50),
              "Invalid firmware readbacks are rejected, not clamped to slider endpoints");
        check(normalizeMonitorInstance(L"\\\\?\\DISPLAY#AUO1234#4&ABC&0&UID100#{GUID}")==
              normalizeMonitorInstance(L"DISPLAY\\AUO1234\\4&ABC&0&UID100_0"),
            "WMI panel identity must match its Windows display interface");
        check(normalizeMonitorInstance(L"DISPLAY\\AUO1234\\INSTANCE1_0")!=
              normalizeMonitorInstance(L"DISPLAY\\AUO1234\\INSTANCE2_0"),
            "Identical monitor models must remain separate WMI targets");
        check(normalizeMonitorInstance(L"DISPLAY\\AUO1234\\INSTANCE_TEST")==L"DISPLAY\\AUO1234\\INSTANCE_TEST",
            "WMI identity matching must preserve nonnumeric instance suffixes");
        check(ddcRawValue(20,220,0)==20 && ddcRawValue(20,220,100)==220,"DDC native range endpoints");
        check(ddcRawValue(20,220,50)==120,"DDC maps percentages into nonzero hardware ranges");
        check(ddcRawValue(20,220,-10)==20 && ddcRawValue(20,220,150)==220,"DDC clamps out-of-range requests");
        check(ddcPercentage(20,220,120)==50,"DDC reads hardware values as percentages");
        check(ddcPercentage(20,220,0)==0 && ddcPercentage(20,220,300)==100,"DDC clamps readback");
        check(ddcRawValue(50,50,90)==50 && ddcPercentage(50,50,50)==0,"DDC handles unsupported zero ranges");
        check(ddcRawValue(0,MAXDWORD,100)==MAXDWORD && ddcPercentage(0,MAXDWORD,MAXDWORD)==100,"DDC range conversion cannot overflow");
        {
            DdcController ddc;
            check(!ddc.set(L"missing",DdcFeature::Brightness,60),"DDC refuses writes before successful discovery");
            ddc.seed(L"NightLight-test-no-such-display",23,67);
            ddc.seed(L"NightLight-test-second-display",81,42);
            ddc.seed(L"NightLight-test-no-such-display",50,50);
            check(ddc.state(L"NightLight-test-no-such-display").brightness.percent==23,
                  "Rebuilding controls must not replace their latest cached positions");
            ddc.refresh(L"NightLight-test-no-such-display");
            for (int attempt=0;attempt<500 && ddc.state(L"NightLight-test-no-such-display").loading;++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto missing=ddc.state(L"NightLight-test-no-such-display");
            check(!missing.loading && !missing.brightness.supported && !missing.contrast.supported,
                  "Missing displays must report DDC unavailable without touching another display");
            check(missing.brightness.hasValue && missing.brightness.percent==23 &&
                  missing.contrast.hasValue && missing.contrast.percent==67,
                  "Unavailable readbacks retain each feature's cached percentage");
            const auto other=ddc.state(L"NightLight-test-second-display");
            check(other.brightness.percent==81 && other.contrast.percent==42,
                  "Refreshing one display must preserve the other display's cached controls");
            check(!ddc.set(L"NightLight-test-no-such-display",DdcFeature::Brightness,60),
                  "DDC must never fall back to a different physical display");
            ddc.restore(L"NightLight-test-no-such-display",23,67,true,false);
            for (int attempt=0;attempt<500 && ddc.state(L"NightLight-test-no-such-display").brightness.pending;++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto restored=ddc.state(L"NightLight-test-no-such-display");
            check(restored.brightness.managed && restored.brightness.percent==23 &&
                  restored.brightness.failed && !restored.brightness.pending,
                  "A disconnected monitor retains its restoration target and reports the failed attempt");
            check(restored.contrast.generation==0 && !restored.contrast.managed && restored.contrast.percent==67,
                  "Recovery must never write an unknown feature's placeholder percentage");
            check(ddc.state(L"NightLight-test-second-display").brightness.percent==81,
                  "Recovery must never retarget another monitor");
            ddc.invalidateTopology();
            check(!ddc.state(L"NightLight-test-no-such-display").brightness.hasValue &&
                  !ddc.state(L"NightLight-test-second-display").brightness.hasValue,
                  "A topology change invalidates every cached GDI-targeted DDC reading");
            ddc.seed(L"NightLight-test-no-such-display",23,67,true,true);
            check(ddc.state(L"NightLight-test-no-such-display").brightness.percent==23 &&
                  ddc.state(L"NightLight-test-no-such-display").contrast.percent==67,
                  "Per-screen last applied values can be reseeded after a wake");
            ddc.stop();
            check(!ddc.set(L"missing",DdcFeature::Contrast,40),"DDC refuses writes after shutdown");
        }
        {
            DdcValue target;
            target.percent=23; target.hasValue=true; target.managed=true;
            target.restoring=true; target.failed=true;
            DdcValue wakingDefault;
            wakingDefault.percent=100; wakingDefault.hasValue=true;
            wakingDefault.readable=true; wakingDefault.supported=true;
            applyDdcObservation(target,wakingDefault,false);
            check(target.percent==23 && target.failed && target.restoring,
                "Wake readbacks must not erase a protected saved hardware target");
            verifyDdcRestore(target,wakingDefault);
            check(target.failed && target.percent==23,
                "A successful transport write with a mismatching boot readback requires retry");
            wakingDefault.percent=24;
            verifyDdcRestore(target,wakingDefault);
            check(!target.failed && target.percent==23,
                "Allow a one-point hardware quantization difference without changing the saved target");
            target.pending=true;
            wakingDefault.percent=100;
            verifyDdcRestore(target,wakingDefault);
            check(!target.failed && target.percent==23,
                "Do not verify an in-flight restore against an old hardware read");
            target.pending=false;
            target.restoring=false;
            applyDdcObservation(target,wakingDefault,false);
            check(target.percent==100 && !target.failed,
                "Normal hardware readback remains authoritative after recovery");
        }
        const std::string legacy=R"({"version":"2.1.1","screen":{"kelvin":4200,"brightness":25,"monitor_profiles":{"\\\\.\\DISPLAY1":{"enabled":false,"kelvin":3120,"brightness":85,"custom_name":"Meu ecr\u00e3"}}},"general":{"language":"pt-PT","appearance":"light","startup_mode":"tray","color_schedule":{"enabled":true,"day_start":480,"night_start":1200,"day_kelvin":6200,"night_kelvin":3200,"transition_minutes":45}},"old_module":{"preserve_me":[1,"ok",false]}})";
        auto data=Json::parse(legacy);
        check(data.get("screen").get("kelvin").integer()==4200,"Read old Kelvin");
        check(data.get("old_module").get("preserve_me").isArray(),"Preserve unknown fields");
        check(Json::parse(data.dump()).dump()==data.dump(),"JSON roundtrip");
        check(fromUtf8(toUtf8(L"Ecrã — Dell"))==L"Ecrã — Dell","PT-PT Unicode");
        check(Json::parse(R"("\uD83D\uDE80")").str()==std::string("\xF0\x9F\x9A\x80"),
              "JSON surrogate pairs");
        bool invalid=false;
        try {
 Json::parse("{\"a\":}"); } catch(...) {invalid=true;}
        check(invalid,"Reject invalid JSON");
        check(legacySoftwareCursorOverride(L"-1") &&
              legacySoftwareCursorOverride(L"  -1  "),
              "Recognize the old NightLight -1 software-cursor workaround");
        check(!legacySoftwareCursorOverride(L"0") &&
              !legacySoftwareCursorOverride(L"1") &&
              !legacySoftwareCursorOverride(L"5") &&
              !legacySoftwareCursorOverride(L"") &&
              !legacySoftwareCursorOverride(L"-10"),
              "Hardware pointer repair must preserve ordinary mouse settings");
        const Ramp ruby=buildKelvinRamp(1000);
        const Ramp midday=buildKelvinRamp(6500);
        const Ramp dusk=buildKelvinRamp(3800);
        check(ruby.red[255]==65535 && ruby.green[255]==0 && ruby.blue[255]==0,
              "Upstream 1000K must fully attenuate green and blue without overlays");
        check(midday.red[255]==65535 && midday.green[255]==65535 &&
              midday.blue[255]==65535,"6500K keeps 100% brightness");
        check(dusk.blue[255]<midday.blue[255],"Kelvin attenuates blue");
        check(sameRamp(buildKelvinRamp(400),ruby),"Clamped Kelvin");
        // This fails under the old MSVC ANSI code page, because the UTF-8
        // source literal would become "Ã" + "§" etc. Wide escapes are used
        // only for the EXPECTED string so the test itself is independent of
        // the machine's active Windows system language.
        check(std::wstring(L"Área de notificação · Ecrãs mantêm") ==
              L"\u00C1rea de notifica\u00E7\u00E3o \u00B7 Ecr\u00E3s mant\u00EAm",
              "PT-PT wide source literals must compile as UTF-8");

        const Ramp neutral=buildKelvinRamp(6500);
        for (int channelIndex=0; channelIndex<256; ++channelIndex) {
            const WORD expected=static_cast<WORD>(channelIndex*257);
            check(neutral.red[channelIndex]==expected &&
                  neutral.green[channelIndex]==expected &&
                  neutral.blue[channelIndex]==expected,
                  "6500K must be identity at every LUT index");
        }
        Ramp previous=buildKelvinRamp(1000);
        for (int kelvin=1010; kelvin<=6500; kelvin+=10) {
            const Ramp next=buildKelvinRamp(kelvin);
            for (int index=0; index<256; ++index) {
                check(next.red[index]==previous.red[index],
                      "Red brightness must stay at 100 percent");
                check(next.green[index]>=previous.green[index],
                      "Green channel must never darken when increasing Kelvin");
                check(next.blue[index]>=previous.blue[index],
                      "Blue channel must never darken when increasing Kelvin");
                check(next.red[index]>=next.green[index] &&
                      next.green[index]>=next.blue[index],
                      "RGB channel gains must remain consistently ordered");
            }
            previous=next;
        }
        {
            GammaRampState state,otherState;
            Ramp hardware=buildKelvinRamp(6500),otherHardware=hardware;
            int writes=0,otherWrites=0;
            auto read=[&](Ramp& out){out=hardware;return true;};
            auto write=[&](const Ramp& in){hardware=in;++writes;return true;};
            auto readOther=[&](Ramp& out){out=otherHardware;return true;};
            auto writeOther=[&](const Ramp& in){otherHardware=in;++otherWrites;return true;};
            const Ramp fixed=buildKelvinRamp(1900);
            check(updateGammaRamp(fixed,GammaRefresh::Normal,otherState,readOther,writeOther),"Initial second-monitor warmth");
            for (int k=1800;k<=2000;k+=10) {
                const Ramp target=buildKelvinRamp(k);
                check(updateGammaRamp(target,GammaRefresh::Normal,state,read,write),"Continuous warm gamma updates");
                check(sameRamp(hardware,target),"Warm drag must send the target directly without neutral intermediate frames");
                check(updateGammaRamp(fixed,GammaRefresh::Normal,otherState,readOther,writeOther),"Keep other display warm");
            }
            check(otherWrites==1,"Dragging one display must not rewrite the unchanged display's gamma");
            const Ramp target=hardware;
            const int before=writes;
            check(updateGammaRamp(target,GammaRefresh::Verify,state,read,write) && writes==before,
                  "Idle verification must not rewrite an already applied ramp");
            hardware=buildKelvinRamp(6500);
            check(updateGammaRamp(target,GammaRefresh::Verify,state,read,write) && writes==before+1 && sameRamp(hardware,target),
                  "Watchdog restores temperature after a silent driver reset");
            check(updateGammaRamp(target,GammaRefresh::Restore,state,read,write) && writes==before+2,
                  "Wake recovery reasserts the saved ramp even if the driver reports a cached value");
            check(!updateGammaRamp(target,GammaRefresh::Restore,state,read,[](const Ramp&){return false;}),
                  "Failed wake writes cannot be cached as success");
            check(updateGammaRamp(target,GammaRefresh::Normal,state,read,write) && writes==before+3,
                  "An initially failed restore remains retryable");
        }
        const Ramp previouslyWarm=buildKelvinRamp(2500);
        check(sameRamp(buildAppliedRamp(6500,true,&previouslyWarm),neutral),
              "6500K while enabled must not restore tinted startup gamma");
        check(sameRamp(buildAppliedRamp(4000,false,&previouslyWarm),previouslyWarm),
              "Disabled filter must restore the captured calibration");
        check(sameRamp(buildAppliedRamp(6500,false,nullptr),neutral),
              "Disabled filter without baseline uses identity");
        // A Windows wake can swap DISPLAY1/DISPLAY2, but not the real
        // physical paths. Verify that identity, names and stored targets
        // follow the panel rather than the transient GDI source number.
        {
            Monitor left,right;
            left.device=L"\\\\.\\DISPLAY1";
            left.physicalId=L"\\\\?\\DISPLAY#DEL123#PANEL_LEFT#{GUID}";
            left.windowsName=L"Dell";left.alias=L"Work";
            left.kelvin=3200;
            right.device=L"\\\\.\\DISPLAY2";
            right.physicalId=L"\\\\?\\DISPLAY#DEL123#PANEL_RIGHT#{GUID}";
            right.windowsName=L"Dell";right.alias=L"Gaming";
            right.kelvin=4700;
            Monitor newLeft=left,newRight=right;
            newLeft.device=right.device;
            newRight.device=left.device;
            check(monitorProfileKey(left)==monitorProfileKey(newLeft) &&
                  monitorProfileKey(right)==monitorProfileKey(newRight) &&
                  monitorProfileKey(left)!=monitorProfileKey(right),
                "Physical monitor keys survive swapped GDI names, even for identical models");
            check(samePhysicalMonitor(left,newLeft) &&
                  !samePhysicalMonitor(left,newRight),
                "Recovery matches the physical panel, not a reused DISPLAYx");
            check(wakeTopologyIsUsable({left,right},{newLeft,newRight}),
                "Valid two-monitor wake with swapped GDI numbers remains recoverable");
            check(!wakeTopologyIsUsable({left,right},{newLeft}),
                "One missing monitor is not accepted as a complete wake topology");
            Monitor unknown=newRight;
            unknown.physicalId.clear();
            check(!samePhysicalMonitor(right,unknown) &&
                  !wakeTopologyIsUsable({left,right},{newLeft,unknown}),
                "An unresolved physical identity cannot steal the last display's values");
            Monitor duplicate=newRight;
            duplicate.physicalId=newLeft.physicalId;
            check(!wakeTopologyIsUsable({left,right},{newLeft,duplicate}),
                "Duplicate physical identities must never share a monitor profile");
            Monitor moved=newLeft;
            moved.windowsName=L"Temporary Generic Monitor";
            check(samePhysicalMonitor(left,moved),
                "A temporary Windows display label must not change physical identity");
        }
        Monitor first, second;
        first.windowsName=L"Asus 60HZ"; first.kelvin=1900;
        second.windowsName=L"Dell"; second.alias=L"Portrait"; second.kelvin=1900;
        const Ramp single=buildAppliedRamp(first.kelvin,first.enabled,nullptr);
        check(sameRamp(single,buildKelvinRamp(1900)),
            "Single-display warmth uses the per-monitor gamma ramp, not a fullscreen matrix");
        const Ramp untouched=buildAppliedRamp(second.kelvin,second.enabled,nullptr);
        for (int k=1800;k<=2000;k+=10) {
            first.kelvin=k;
            check(sameRamp(buildAppliedRamp(first.kelvin,first.enabled,nullptr),
                           buildKelvinRamp(k)) &&
                  sameRamp(buildAppliedRamp(second.kelvin,second.enabled,nullptr),untouched),
                "One monitor changes its independent gamma LUT without retinting the other");
        }
        first.kelvin=1900;
        second.kelvin=4200;
        check(sameRamp(buildAppliedRamp(second.kelvin,second.enabled,nullptr),
                       buildKelvinRamp(4200)),
            "Different monitor temperatures use their own independent hardware LUTs");
        second.kelvin=1900; second.enabled=false;
        check(sameRamp(buildAppliedRamp(second.kelvin,second.enabled,nullptr),
                       buildKelvinRamp(6500)),
            "Disabling one display restores neutral gamma without a fullscreen colour matrix");
        const auto tip=monitorTooltip({first,second});
        check(tip.find(L"Asus 60HZ: 1900 K")!=std::wstring::npos &&
              tip.find(L"Portrait: 1900 K (off)")!=std::wstring::npos,
              "Tooltip contains each monitor name, Kelvin and disabled state");
        first.alias=std::wstring(64,L'A'); second.alias=std::wstring(64,L'B');
        check(monitorTooltip({first,second}).size()>127,"Full tooltip must preserve long monitor names");
        check(buildKelvinRamp(1900).blue[255]==0,"Upstream 1900K removes blue");
        const RECT track{100,200,500,212};
        const RECT hit=sliderHitRect(track,12,30);
        check(hit.left==88 && hit.right==512 &&
              hit.top==170 && hit.bottom==242,
              "Slider needs a tall, DPI-scalable click region");
        // Each monitor tab fits its complete name and its independent switch,
        // with no standalone Enabled button or truncated-label width cap.
        // Opening fade and upward translation use exactly the same
        // 150ms motion, ending at the saved resting window coordinates.
        check(kScreenOffActionDelayMs==250 && kScreenOffInputGuardMs==3000,
            "Screen-off waits 250ms for the click release then protects input for 3 seconds");
        check(kScreenOffButtonSizeDip==32,
            "Screen-off header action matches the refresh button size");
        check(kScreenOffMinCorrectionIntervalMs==450 &&
              kScreenOffMaxCorrections==8,
            "Event-driven screen-wake corrections are capped at 8 and 450ms apart");
        check(shouldCorrectScreenWake(1000,0,0) &&
              shouldCorrectScreenWake(1450,1000,1),
            "The initial physical wake and an event after a full cooldown can be corrected");
        check(!shouldCorrectScreenWake(1449,1000,1) &&
              !shouldCorrectScreenWake(999,1000,1),
            "Burst input and nonmonotonic timestamps do not cause repeated screen-off");
        check(!shouldCorrectScreenWake(3000,1000,8) &&
              shouldCorrectScreenWake(3000,1000,7),
            "At most eight reactive corrections are permitted per screen-off activation");
        check(Preferences{}.theme==L"system",
            "Fresh installations default to following Windows");
        check(effectiveDarkTheme(L"system",true) &&
              !effectiveDarkTheme(L"system",false),
            "Follow system uses Windows app colour preference");
        check(effectiveDarkTheme(L"dark",false) &&
              effectiveDarkTheme(L"dark",true) &&
              !effectiveDarkTheme(L"light",true) &&
              !effectiveDarkTheme(L"light",false),
            "Manual dark and light choices override the Windows preference");
        const std::vector<int> tabs{260,250,270};
        check(kCardOuterMarginDip==16 &&
              kCardsTopMarginDip==kCardOuterMarginDip &&
              kCardsGapDip==12 &&
              kCardInnerInsetDip==16,
            "Outside margins remain 16 DIP; all inter-section gaps are 12 DIP");
        check(kCardCornerRadiusDip==8 &&
              kCardCornerRadiusDip*2==16,
            "All section cards use an explicit 8 DIP radius (16 DIP arc diameter)");
        const auto singleVisible=calculateWindowContentMinimum(96,980,
            {200},1,true,true);
        const auto singleHidden=calculateWindowContentMinimum(96,980,
            {200},1,true,false);
        check(singleVisible.screenRows==1 && singleVisible.clientHeight==
            kCardsTopMarginDip+(46+38)+kCardsGapDip+
            kTemperatureUnlockedHeightDip+kCardsGapDip+
            (kDdcSectionBaseDip+kDdcRowStepDip)+kCardOuterMarginDip,
            "Visible height respects 16 DIP outside margins and 12 DIP section gaps");
        check(singleHidden.clientHeight==
            kCardsTopMarginDip+(46+38)+kCardsGapDip+
            kTemperatureUnlockedHeightDip+kCardOuterMarginDip &&
            singleVisible.clientHeight-singleHidden.clientHeight==
                kCardsGapDip+kDdcSectionBaseDip+kDdcRowStepDip,
            "Hidden DDC removes only its card and the intervening 12-DIP gap");
        const auto noButtonWrap=calculateWindowContentMinimum(
            96,450,{160,160},1,false,true);
        const auto headerButton=calculateWindowContentMinimum(
            96,418,{160,160},1,false,true);
        const auto wrappedDisplays=calculateWindowContentMinimum(
            96,380,{160,160},1,false,true);
        const auto compactFits=calculateWindowContentMinimum(
            96,424,{160,160},1,false,true);
        check(noButtonWrap.screenRows==1 &&
              compactFits.screenRows==1 &&
              headerButton.screenRows==1 &&
              headerButton.clientHeight==noButtonWrap.clientHeight &&
              wrappedDisplays.screenRows==2 &&
              wrappedDisplays.clientHeight-noButtonWrap.clientHeight==38,
            "Header actions do not cause monitor-row wrapping; actual displays still wrap");
        const auto wide=calculateWindowContentMinimum(96,980,tabs,2,false,true);
        const auto narrow=calculateWindowContentMinimum(96,420,tabs,2,false,true);
        check(wide.screenRows==1 && narrow.screenRows>=2,
            "Narrow window wraps complete screen tabs into additional rows");
        check(narrow.clientHeight>wide.clientHeight,
            "Wrapping full-name screen tabs increases minimum content height");
        check(narrow.clientWidth>=420 && narrow.clientHeight>0,
            "Minimum window dimensions remain nonzero");
        const auto noDdc=calculateWindowContentMinimum(96,420,tabs,2,false,false);
        check(noDdc.clientHeight<narrow.clientHeight,
            "Expanded DDC rows require a taller window");
        const auto moreDdc=calculateWindowContentMinimum(96,420,tabs,3,false,true);
        check(moreDdc.clientWidth==narrow.clientWidth,
            "Additional DDC monitor rows do not force a wider window");
        check(moreDdc.clientHeight-narrow.clientHeight==kDdcRowStepDip,
            "Each additional monitor adds exactly 110 DIP including the +10 DIP gap");
        check(kDdcFirstRowTopDip==46 &&
              kDdcFirstRowTopDip-32==14,
            "DDC heading-to-first-monitor gap increased by exactly 10 DIP");
        check(kDdcSectionBaseDip==34 && kDdcRowStepDip==110 &&
              kDdcFirstRowTopDip+66+28<=kDdcSectionBaseDip+kDdcRowStepDip,
            "DDC card grows with taller rows and preserves bottom padding");
        check(kDdcRowStepDip-66-28==16,
            "Contrast slider to next monitor has 16 DIP, 10 more than before");
        check(kDdcFirstRowTopDip+66+28<
              kDdcFirstRowTopDip+kDdcRowStepDip,
            "Taller DDC monitor rows never overlap");
        const auto twoMonitorsWide=calculateWindowContentMinimum(96,1600,tabs,2,false,true);
        const auto threeMonitorsWide=calculateWindowContentMinimum(96,1600,tabs,3,false,true);
        check(threeMonitorsWide.clientHeight>twoMonitorsWide.clientHeight,
            "DDC controls stack monitor rows vertically rather than horizontally");
        check(threeMonitorsWide.clientWidth==twoMonitorsWide.clientWidth,
            "DDC stacking preserves the width regardless of monitor count");
        check(twoMonitorsWide.screenRows==1 && threeMonitorsWide.screenRows==1,
            "DDC row count does not alter Screen tab wrapping");
        const auto unlocked=calculateWindowContentMinimum(96,420,tabs,2,true,true);
        check(unlocked.clientHeight<narrow.clientHeight,
            "Gamma-unlocked layout uses shorter temperature section");
        const auto unlockedWide=calculateWindowContentMinimum(96,980,tabs,2,true,true);
        check(wide.clientHeight-unlockedWide.clientHeight==
                kTemperatureLockedHeightDip-kTemperatureUnlockedHeightDip,
            "Unlocked gamma state removes only the space for the unlock button");
        check(kTemperatureLockedHeightDip==112 &&
              kTemperatureUnlockedHeightDip==80 &&
              kTemperatureGammaTopDip+29<=kTemperatureLockedHeightDip-8,
            "Temperature card is 14 DIP shorter while leaving 8 DIP under gamma button");
        // Track is 4 DIP high and its thumb is 20 DIP across. The label
        // text starts 2 DIP after the thumb and ends with a 6 DIP bottom inset.
        check(kTemperatureSliderTopDip+2+10+2<=kTemperatureScaleTopDip &&
              kTemperatureScaleTopDip+18<=kTemperatureUnlockedHeightDip-6 &&
              kTemperatureScaleTopDip+18<kTemperatureGammaTopDip,
            "Temperature thumb, endpoint labels and gamma button never overlap");
        check(kTemperatureNarrowExtraDip==24 &&
              kTemperatureSliderTopDip+kTemperatureNarrowExtraDip-8>=56,
            "Narrow layout clears the Kelvin value's second line");
        for (int clientWidth:{200,350,560,1024}) {
            for (int dpi:{96,144,192}) {
                const int trackWidth=temperatureSliderWidth(dpi,clientWidth);
                const RECT track{(clientWidth-trackWidth)/2,42,
                    (clientWidth-trackWidth)/2+trackWidth,46};
                const auto labels=temperatureScaleLabelRects(track,56,18);
                const auto kelvinValue=temperatureValueRect(track,100,8,24);
                check(kelvinValue.right==track.right &&
                      kelvinValue.top==8 && kelvinValue.bottom==32 &&
                      kelvinValue.left>=track.left,
                    "Kelvin value right edge always coincides with slider endpoint");
                check(labels.low.left==track.left &&
                      labels.high.right==track.right &&
                      labels.low.right==labels.high.left,
                    "1000 K and 6500 K remain flush with exact slider endpoints");
                check(labels.low.top==labels.high.top &&
                      labels.low.bottom==labels.high.bottom,
                    "Both Kelvin labels share the same baseline at any width and DPI");
                check(labels.low.right>labels.low.left &&
                      labels.high.right>labels.high.left,
                    "Even the 200px window gives both endpoint labels room");
            }
        }
        const auto narrowOneLine=calculateWindowContentMinimum(96,349,tabs,2,false,false);
        const auto wideOneLine=calculateWindowContentMinimum(96,350,tabs,2,false,false);
        check(narrowOneLine.clientHeight>wideOneLine.clientHeight,
            "Narrow temperatures retain an extra row for the Kelvin value");
        const auto scaled=calculateWindowContentMinimum(144,630,
            {270,262,278},2,false,true);
        check(scaled.clientWidth>=630 && scaled.clientHeight>narrow.clientHeight,
            "Minimum dimensions scale with display DPI");
        const auto oversized=calculateWindowContentMinimum(96,420,
            {1500},0,false,false);
        check(oversized.clientWidth==420 && oversized.screenRows==1,
            "Long monitor names ellipsize within one row; screen-off stays in the header");
        const auto tiny=calculateWindowContentMinimum(96,180,tabs,2,false,true);
        check(tiny.clientWidth==180 && tiny.screenRows==3,
            "At minimum width only the three monitor tabs wrap; header actions stay above them");
        // Both header actions are independent of the monitor rows, even at
        // narrow widths and fractional DPI scales. Long names must not add
        // an extra row or enlarge the window when their text is clipped.
        for (int dpi:{96,120,144,192}) {
            for (int widthDip:{180,200,420,980}) {
                const int width=MulDiv(widthDip,dpi,96);
                const auto one=calculateWindowContentMinimum(dpi,width,
                    {MulDiv(1500,dpi,96)},0,false,false);
                const auto empty=calculateWindowContentMinimum(dpi,width,{},0,false,false);
                const auto three=calculateWindowContentMinimum(dpi,width,
                    {MulDiv(1500,dpi,96),MulDiv(1500,dpi,96),MulDiv(1500,dpi,96)},0,false,false);
                check(one.clientWidth==width && one.screenRows==1 &&
                      one.clientHeight==empty.clientHeight,
                    "Header actions do not add a row at any supported DPI or width");
                check(three.clientWidth==width && three.screenRows==3 &&
                      three.clientHeight-one.clientHeight==MulDiv(46+3*38,dpi,96)-MulDiv(46+38,dpi,96),
                    "Only actual monitor tabs contribute additional rows at any DPI");
            }
        }
        check(tiny.clientHeight>narrow.clientHeight,
            "Compact temperature layout receives additional vertical clearance");
        check(temperatureSliderWidth(96,200)==128 &&
              temperatureSliderWidth(96,800)==728,
            "Temperature slider tracks the symmetric 16 DIP outside margins");
        // Reserve the width of the longest percentage once. This avoids
        // changing the trackbar's geometry when 99 % becomes 100 %.
        check(ddcSliderWidth(96,200,30)==78 &&
              ddcSliderWidth(96,800,30)==678,
            "DDC trackbars retain right-aligned percentages inside the new margins");
        check(temperatureSliderWidth(144,300)==192 &&
              ddcSliderWidth(144,300,45)==117,
            "DDC alignment and 16 DIP symmetric outside margins scale with DPI");
        for (int dpi:{96,120,144,192}) {
            const auto dip=[dpi](int v) { return MulDiv(v,dpi,96); };
            for (int widthDip:{200,350,560,1024}) {
                const int clientWidth=dip(widthDip);
                const int maxTextWidth=dip(30); // measured "100 %" sample
                const int trackWidth=temperatureSliderWidth(dpi,clientWidth);
                const int trackLeft=(clientWidth-trackWidth)/2;
                const int sliderRight=trackLeft+trackWidth;
                const int sliderLeft=dip(kCardOuterMarginDip+
                    kCardInnerInsetDip+kDdcSliderLeftInsetDip);
                const RECT percent=ddcPercentageRect(dpi,clientWidth,
                    maxTextWidth,42,dip(28));
                const int width=ddcSliderWidth(dpi,clientWidth,maxTextWidth);
                check(percent.right==sliderRight,
                    "Brightness and contrast percentage right edges equal 6500 K's endpoint");
                check(percent.left==percent.right-maxTextWidth &&
                      percent.top==42 && percent.bottom==42+dip(28),
                    "Measured 100 percent field keeps a fixed width and correct height");
                check(width>0 && sliderLeft+width==percent.left,
                    "DDC trackbar ends at the percentage field start, preserving the gap");
                check(width>=dip(16),
                    "DDC slider remains usable at the 200-DIP minimum window width");
            }
        }
        for (int width:{201,355,561,801}) {
            const RECT percent=ddcPercentageRect(96,width,31,0,28);
            const int sliderWidth=temperatureSliderWidth(96,width);
            check(percent.right==(width-sliderWidth)/2+sliderWidth,
                "Odd window sizes keep percentages exactly aligned with Kelvin labels");
        }
        // Initial silent startup must not activate another window.
        // Interactive show/hide is handled by Win32/DWM.
        check((kHideToTrayFlags&SWP_HIDEWINDOW)!=0 &&
              (kHideToTrayFlags&SWP_NOACTIVATE)!=0,
            "Initial tray-only launch hides without activating another window");
        check((kHideToTrayFlags&SWP_NOMOVE)!=0 &&
              (kHideToTrayFlags&SWP_NOSIZE)!=0 &&
              (kHideToTrayFlags&SWP_NOZORDER)!=0 &&
              (kHideToTrayFlags&SWP_NOOWNERZORDER)!=0,
            "Hiding preserves saved position, size, z-order and hidden owner");
        check((kHideToTrayFlags&SWP_SHOWWINDOW)==0 &&
              (kHideToTrayFlags&SWP_FRAMECHANGED)==0,
            "The hide path cannot accidentally show or reframe the window");
        // Tray selection must toggle on every physical down, including the
        // second press of a double-click, but not again on its release.
        nightlight::TrayClickFilter tray;
        using Click=nightlight::TrayClickEvent;
        check(tray.shouldToggle(Click::MouseDown,1000),
            "First left mouse press opens or hides the tray window immediately");
        check(!tray.shouldToggle(Click::MouseUp,1040),
            "Mouse-up after a handled press cannot toggle the window twice");
        check(!tray.shouldToggle(Click::Select,1041),
            "NIN_SELECT following mouse-up is a duplicate activation");
        check(tray.shouldToggle(Click::MouseDoubleClick,1130),
            "Second fast press in a double-click always toggles immediately");
        check(!tray.shouldToggle(Click::MouseUp,1160) &&
              !tray.shouldToggle(Click::Select,1161),
            "Double-click's trailing release and select cannot undo the toggle");
        check(tray.shouldToggle(Click::MouseDown,1200) &&
              !tray.shouldToggle(Click::MouseUp,1210),
            "Third fast physical click is never throttled");
        check(tray.shouldToggle(Click::KeySelect,1220),
            "Keyboard activation works independently of prior mouse activity");
        nightlight::TrayClickFilter selectOnly;
        check(selectOnly.shouldToggle(Click::Select,5000) &&
              selectOnly.shouldToggle(Click::Select,5015),
            "Shells emitting only NIN_SELECT handle rapid repeated clicks");
        nightlight::TrayClickFilter upOnly;
        check(upOnly.shouldToggle(Click::MouseUp,5000) &&
              !upOnly.shouldToggle(Click::Select,5002) &&
              upOnly.shouldToggle(Click::MouseUp,5200),
            "Legacy mouse-up fallback avoids duplicate Shell selection");
        nightlight::TrayClickFilter swapped;
        check(swapped.shouldToggle(Click::MouseDown,5000) &&
              !swapped.shouldToggle(Click::Select,5001) &&
              !swapped.shouldToggle(Click::MouseUp,5015),
            "Reordered Shell selection and release stay deduplicated");
        // WS_VISIBLE stays set when a window is minimized. A visible
        // iconic HWND must count as closed so the next click restores it.
        check(!trayWindowPresented(false,false) &&
              !trayWindowPresented(true,true) &&
              !trayWindowPresented(false,true) &&
              trayWindowPresented(true,false),
              "Tray presentation distinguishes normal, hidden and iconic states");
        // A burst of physical clicks must not be gated by a prior animation.
        nightlight::TrayClickFilter burst;
        bool shown=false;
        for(unsigned long i=0;i<100;++i) {
            const unsigned long time=6000+i*8;
            const auto press=(i%2)?Click::MouseDoubleClick:Click::MouseDown;
            if(burst.shouldToggle(press,time)) shown=!shown;
            check(shown==(i%2==0),"Every rapid tray press reverses desired visibility");
            check(!burst.shouldToggle(Click::MouseUp,time+2) &&
                  !burst.shouldToggle(Click::Select,time+3),
                  "Rapid releases and Shell selects never undo the press");
        }
        check(!shown,"An even burst of clicks ends at the original visibility");
        wchar_t tmp[MAX_PATH]{};
        check(GetTempPathW(MAX_PATH,tmp)>0,"Temp path");
        const fs::path root=fs::path(tmp)/
            (L"NightLight_native_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::create_directories(root,ec);
        check(!ec,"Create folder");
        const fs::path cfg=root/L"config.json";
        HANDLE file=CreateFileW(cfg.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,nullptr);
        check(file!=INVALID_HANDLE_VALUE,"Open portable config for writing");
        DWORD written=0;
        check(WriteFile(file,legacy.data(),static_cast<DWORD>(legacy.size()),&written,nullptr),
              "Write legacy config");
        CloseHandle(file);
        {
            Model model(root);
            check(model.load(),"Load local portable config");
            check(model.preferences().locale==L"pt-PT","PT language");
            check(model.preferences().theme==L"light","Light theme");
            const auto& migratedSchedule=model.preferences().colorSchedule;
            check(migratedSchedule.enabled && migratedSchedule.mode==ScheduleMode::Custom &&
                  migratedSchedule.points.size()==2 && migratedSchedule.points[0].minute==480 &&
                  migratedSchedule.points[0].kelvin==6200 && migratedSchedule.points[1].minute==1200 &&
                  migratedSchedule.points[1].kelvin==3200 && migratedSchedule.transitionMinutes==45,
                  "Migrate legacy day/night schedule into editable points");
            check(!model.windowPosition().has_value(),"Legacy config without a window position keeps default placement");
            check(!model.windowSize().has_value(),"Legacy config without dimensions keeps automatic first-run sizing");
            check(!model.inactiveWindowGeometry(),"Legacy config has no inactivity window geometry");
            const RECT dimRect{-1700,180,-1100,720};
            check(model.rememberInactiveWindowGeometry(dimRect),"Remember independent inactivity window geometry on left monitor");
            check(!model.rememberInactiveWindowGeometry(dimRect),"Identical inactivity window geometry is not dirty");
            check(!model.rememberInactiveWindowGeometry(RECT{0,0,100,100}),"Reject unusable inactivity window size");
            check(!model.rememberInactiveWindowGeometry(RECT{0,0,20000,20000}),"Reject excessive inactivity window size");
            check(!model.rememberInactiveWindowGeometry(RECT{-2000000,0,-1999400,540}),"Reject invalid inactivity window coordinates");
            check(model.rememberWindowPosition(-1900,235),"Remember position on monitor left of primary");
            check(model.rememberWindowSize(870,610),"Remember user's outer window width and height");
            check(!model.rememberWindowSize(870,610),"Identical window size should not trigger a config update");
            check(model.rememberWindowSize(200,610),
                "Accept the new 200-pixel minimum width");
            check(model.rememberWindowSize(870,610),
                "Restore the normal geometry after checking the minimum");
            check(!model.rememberWindowSize(199,610) && !model.rememberWindowSize(870,200),
                "Reject sizes narrower than 200 pixels or shorter than 260");
            check(!model.rememberWindowSize(20000,610) && !model.rememberWindowSize(870,20000),
                "Reject unreasonable saved window dimensions");
            check(!model.rememberWindowPosition(-1900,235),"Unchanged window location requires no new write");
            Preferences updated=model.preferences();
            updated.locale=L"en-US";
            updated.theme=L"system";
            check(!updated.dimInactive,"Inactivity dimming defaults to disabled");
            updated.ddcControls=false;updated.colorSchedule={true,{{360,6000},{720,4500},{1200,3000}},ScheduleMode::Custom,20};
            updated.hotkeys[L""]={0x0241,0,0x0242,0};
            updated.hotkeySteps[L""]={7,8,150,200};updated.hotkeySteps[L"MONITOR-A"]={2,3,50,80};
            updated.colorSchedule.locationName=L"Lisbon, Portugal";updated.colorSchedule.latitude=38.722;updated.colorSchedule.longitude=-9.139;
            updated.colorSchedule.points[0].kind=SchedulePointKind::Sunrise;updated.colorSchedule.points[0].customTime=true;
            updated.dimInactive=true;updated.dimAmount=65;updated.dimMinutes=7;
            updated.dimAll=false;updated.dimDisplays.insert(L"PHYSICAL-A");
            model.updatePreferences(updated);
            check(model.preferences().theme==L"system",
                "System-following theme remains a distinct persisted preference");
            check(model.lastError().empty(),"Persist preferences");
            check(model.hardwarePercent(L"test-display-1",false)==50,"Unseen controls have a numeric default");
            check(!model.hasHardwarePercent(L"test-display-1",false),"Unknown UI defaults are not eligible for hardware restoration");
            model.rememberHardwarePercent(L"test-display-1",false,23);
            model.rememberHardwarePercent(L"test-display-1",true,67);
            model.rememberHardwarePercent(L"test-display-2",false,81);
            model.rememberHardwarePercent(L"test-display-2",true,42);
            check(model.hasHardwarePercent(L"test-display-1",false),"Recorded hardware targets are eligible for wake restoration");
            check(model.save(),"Save independent hardware slider values");
        }
        {
            Model model(root);
            check(model.load(),"Reload from same folder");
            check(model.preferences().locale==L"en-US","Saved language");
            check(!model.preferences().ddcControls && model.preferences().colorSchedule.enabled &&
                  model.preferences().colorSchedule.points.size()==3 && model.preferences().colorSchedule.points.front().minute==360 &&
                  model.preferences().colorSchedule.mode==ScheduleMode::Custom && model.preferences().colorSchedule.transitionMinutes==20 &&
                  model.preferences().hotkeys.at(L"")[0]==0x0241,"Schedule and global hotkeys round-trip");
            check(hotkeyStepsFor(model.preferences().hotkeySteps,L"")[2]==150 &&
                  hotkeyStepsFor(model.preferences().hotkeySteps,L"MONITOR-A")[3]==80 &&
                  model.preferences().colorSchedule.locationName==L"Lisbon, Portugal" &&
                  std::abs(model.preferences().colorSchedule.latitude-38.722)<0.001 &&
                  model.preferences().colorSchedule.points.front().kind==SchedulePointKind::Sunrise &&
                  model.preferences().colorSchedule.points.front().customTime,"Location, solar overrides and hotkey increments round-trip");
            check(model.preferences().theme==L"system",
                "System-following theme round-trips through portable config");
            check(model.preferences().dimInactive && model.preferences().dimAmount==65 &&
                  model.preferences().dimMinutes==7 && !model.preferences().dimAll &&
                  model.preferences().dimDisplays.count(L"PHYSICAL-A")==1,
                  "Inactivity settings and physical monitor selection round-trip");
            const auto dimRect=model.inactiveWindowGeometry();
            check(dimRect && dimRect->left==-1700 && dimRect->top==180 &&
                  dimRect->right==-1100 && dimRect->bottom==720,
                  "Restore independent inactivity geometry through a restart");
            const auto restoredWindow=model.windowPosition();
            check(restoredWindow && restoredWindow->x==-1900 && restoredWindow->y==235,
                "Persist and restore screen position across application starts");
            const auto restoredSize=model.windowSize();
            check(restoredSize && restoredSize->cx==870 && restoredSize->cy==610,
                "Restore saved window dimensions across application starts");
            check(model.rememberWindowSize(200,610) && model.save(),
                "Save 200-pixel window geometry for the next launch");
            check(model.hardwarePercent(L"test-display-1",false)==23 &&
                  model.hardwarePercent(L"test-display-1",true)==67 &&
                  model.hardwarePercent(L"test-display-2",false)==81 &&
                  model.hardwarePercent(L"test-display-2",true)==42,
                  "Restart restores separate brightness and contrast percentages for every display");
            HANDLE handle=CreateFileW(cfg.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
                                      OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            check(handle!=INVALID_HANDLE_VALUE,"Read saved config");
            DWORD size=GetFileSize(handle,nullptr),received=0;
            std::string content(size,'\0');
            check(ReadFile(handle,content.data(),size,&received,nullptr),"Read bytes");
            CloseHandle(handle);
            auto saved=Json::parse(content);
            check(saved.get("general").get("appearance").str()=="system",
                "Portable config retains system theme mode");
            check(std::holds_alternative<std::nullptr_t>(
                saved.get("general").get("startup_mode").value),
                "Obsolete startup preferences are removed from saved config");
            check(saved.get("general").get("window_position").get("x").integer()==-1900 &&
                saved.get("general").get("window_position").get("y").integer()==235 &&
                saved.get("general").get("window_position").get("width").integer()==200 &&
                saved.get("general").get("window_position").get("height").integer()==610,
                "Portable config stores signed window coordinates and exact dimensions");
            check(std::holds_alternative<std::nullptr_t>(saved.get("screen").get("brightness").value),
                  "Unused software brightness discarded");
            check(!saved.get("old_module").isObject(),
                  "Legacy modules discarded on save");
            check(std::get<Json::Object>(saved.value).size()==2,"Only current top-level settings are persisted");
            const auto& profiles=saved.get("screen").get("monitor_profiles");
            const auto& values=std::get<Json::Object>(profiles.value);
            bool foundAlias=false;
            for(const auto& item:values) {
                check(std::holds_alternative<std::nullptr_t>(item.second.get("brightness").value),"Discard unused per-monitor software brightness");
                if(item.second.get("custom_name").str()=="Meu ecr\xc3\xa3") {
                    foundAlias=true;check(item.second.get("kelvin").integer()==3120 && !item.second.get("enabled").boolean(),"Migration retains current alias, Kelvin and enabled state");
                }
            }
            check(foundAlias,"Migration retains named disconnected monitor profiles");
        }
        {
            Model model(root);check(model.load(),"Reload config for save idempotence");
            WIN32_FILE_ATTRIBUTE_DATA before{},after{};
            check(GetFileAttributesExW(cfg.c_str(),GetFileExInfoStandard,&before),"Read saved timestamp");
            Sleep(20);check(model.save(),"Repeated unchanged save succeeds");
            check(GetFileAttributesExW(cfg.c_str(),GetFileExInfoStandard,&after) && CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime)==0,
                "Unchanged settings must not rewrite and flush the config");
            check(DeleteFileW(cfg.c_str()),"Simulate externally removed config");
            check(model.save() && fs::exists(cfg),"Cached saves recreate an externally deleted config");
            HANDLE changed=CreateFileW(cfg.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            check(changed!=INVALID_HANDLE_VALUE,"Simulate external config replacement");
            const std::string external="{}";DWORD count=0;check(WriteFile(changed,external.data(),2,&count,nullptr),"Write external config");CloseHandle(changed);
            check(model.save(),"Restore current settings after an external replacement");
            check(GetFileAttributesExW(cfg.c_str(),GetFileExInfoStandard,&after) && after.nFileSizeLow>2,
                "Save cache must not ignore externally changed files");
        }
        {
            const fs::path blocked=root/L"blocked";fs::create_directories(blocked,ec);
            const fs::path path=blocked/L"config.json";
            HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            check(file!=INVALID_HANDLE_VALUE,"Create oversized fixture");
            LARGE_INTEGER size{};size.QuadPart=8*1024*1024+1;
            check(SetFilePointerEx(file,size,nullptr,FILE_BEGIN) && SetEndOfFile(file),"Size oversized config");CloseHandle(file);
            { Model model(blocked);check(!model.load(),"An oversized existing config must never become default settings"); }
            WIN32_FILE_ATTRIBUTE_DATA preserved{};
            check(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&preserved) && preserved.nFileSizeLow==8*1024*1024+1,
                "An unreadable config must remain byte-for-byte intact");
            HANDLE locked=CreateFileW(cfg.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            check(locked!=INVALID_HANDLE_VALUE,"Lock config fixture");
            { Model model(root);check(!model.load(),"Sharing violations cannot reset a valid configuration"); }
            CloseHandle(locked);
        }
        // Every supported language must survive a complete save and reload.
        for(const auto& l:uiLanguages){
            { Model model(root);check(model.load(),"Load before locale switch");auto p=model.preferences();p.locale=l.code;model.updatePreferences(p);check(model.preferences().locale==l.code,"Apply locale"); }
            { Model model(root);check(model.load() && model.preferences().locale==l.code,"Persist locale across restart"); }
        }
        fs::remove_all(root,ec);
        std::wcout<<L"All native regressions passed.\n";
        return 0;
    } catch (const std::exception& err) {
        std::cerr<<"Native regressions failed: "<<err.what()<<"\n";
        return 1;
    }
}
