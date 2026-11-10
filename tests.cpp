#include "core.hpp"
#include "ddc.hpp"
#include "motion.hpp"
#include "wmi_brightness.hpp"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <chrono>
using namespace nightlight;
using portable::Json;
namespace fs=std::filesystem;
void check(bool ok,const char* message) {
    if(!ok) throw std::runtime_error(message);
}
int main() {
    try {
        check(sliderPositionFromX(-1000,10,210,0,10000)==0,"Captured dragging clamps beyond the left edge");
        check(sliderPositionFromX(1000,10,210,0,10000)==10000,"Captured dragging clamps beyond the right edge");
        check(sliderPositionFromX(110,10,210,0,10000)==5000,"Captured dragging maps the pointer continuously");
        check(sliderPositionFromX(110,10,10,0,10000)==0,"Degenerate slider geometry stays safe");
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
        try { Json::parse(std::string(140,'[')+"0"+std::string(140,']')); } catch(const std::exception&) { tooDeep=true; }
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
        DWORD settingWritten=0; int directCalls=0,vcpCalls=0;
        check(writeDdcValue(false,0,0,37,[&](DWORD value){settingWritten=value;++directCalls;return true;},
            [&](DWORD){++vcpCalls;return true;}) && settingWritten==37 && directCalls==1 && vcpCalls==0,
            "Upstream compatibility: failed getters must not block a direct hardware write");
        check(writeDdcValue(true,20,220,50,[](DWORD){return false;},[&](DWORD value){settingWritten=value;return true;}) && settingWritten==120,
            "DDC writes must retain VCP fallback and honor a known hardware range");
        check(!writeDdcValue(false,0,0,45,[](DWORD){return false;},[](DWORD){return false;}),
            "DDC must report failed writes rather than claim success");
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
            ddc.stop();
            check(!ddc.set(L"missing",DdcFeature::Contrast,40),"DDC refuses writes after shutdown");
        }
        const std::string legacy=R"({"version":"2.1.1","screen":{"kelvin":4200,"brightness":25,"monitor_profiles":{"\\\\.\\DISPLAY1":{"enabled":false,"kelvin":3120,"brightness":85,"custom_name":"Meu ecr\u00e3"}}},"general":{"language":"pt-PT","appearance":"light","startup_mode":"tray"},"old_module":{"preserve_me":[1,"ok",false]}})";
        auto data=Json::parse(legacy);
        check(data.get("screen").get("kelvin").integer()==4200,"Read old Kelvin");
        check(data.get("old_module").get("preserve_me").isArray(),"Preserve unknown fields");
        check(Json::parse(data.dump()).dump()==data.dump(),"JSON roundtrip");
        check(fromUtf8(toUtf8(L"Ecrã — Dell"))==L"Ecrã — Dell","PT-PT Unicode");
        check(Json::parse(R"("\uD83D\uDE80")").str()==std::string("\xF0\x9F\x9A\x80"),
              "JSON surrogate pairs");
        bool invalid=false;
        try { Json::parse("{\"a\":}"); } catch(...) {invalid=true;}
        check(invalid,"Reject invalid JSON");
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
        Monitor first, second;
        first.windowsName=L"Asus 60HZ"; first.kelvin=1900;
        second.windowsName=L"Dell"; second.alias=L"Portrait"; second.kelvin=1900;
        check(sharedMatrixKelvin({first})==1900,"Single display uses upstream matrix");
        check(sharedMatrixKelvin({first,second})==0,"Multi-monitor backend stays independent even when temperatures coincide");
        for (int k=1800;k<=2000;k+=10) {
            first.kelvin=k;
            check(sharedMatrixKelvin({first,second})==0,"Dragging across another display's temperature must not toggle the global effect");
        }
        first.kelvin=1900;
        second.kelvin=4200;
        check(sharedMatrixKelvin({first,second})==0,"Different temperatures require independent LUTs");
        second.kelvin=1900; second.enabled=false;
        check(sharedMatrixKelvin({first,second})==0,"Disabled displays must not receive global warmth");
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
        check(kScreenOffInputGuardMs==4000,
            "Screen-off protection has a strict four-second input deadline");
        check(kWindowOpeningDurationMs==150 &&
              kWindowOpeningSlideDip==12 &&
              kWindowOpeningInitialAlpha>0 &&
              kWindowOpeningInitialAlpha<64,
            "Fast 150ms opening slide/fade and perceptible first frame");
        for(int dpi:{96,120,144,192}) {
            const int fullOffset=MulDiv(kWindowOpeningSlideDip,dpi,96);
            const double initialProgress=kWindowOpeningInitialAlpha/255.0;
            check(windowOpeningSlideOffset(dpi,0)==fullOffset &&
                  windowOpeningSlideOffset(dpi,1)==0,
                "Slide begins lower and finishes exactly at resting position");
            check(windowOpeningSlideOffset(dpi,initialProgress)>0,
                "Window opens below target before first visible fade frame");
            check(windowOpeningSlideOffset(dpi,0.5)>0 &&
                  windowOpeningSlideOffset(dpi,0.5)<fullOffset &&
                  windowOpeningSlideOffset(dpi,-1)==fullOffset &&
                  windowOpeningSlideOffset(dpi,2)==0,
                "Slide offset decreases smoothly and clamps at both ends");
            Motion opening;
            opening.snap(initialProgress);
            opening.to(1,1000,kWindowOpeningDurationMs,true);
            check(opening.active(1000) && opening.active(1075) &&
                  !opening.active(1150) && opening.value(1150)==1,
                "Both fade and slide share a single 150ms animation clock");
            check(windowOpeningSlideOffset(dpi,opening.value(1075))>0,
                "Window continues rising while alpha increases");
        }
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
        const auto wrappedButton=calculateWindowContentMinimum(
            96,418,{160,160},1,false,true);
        check(noButtonWrap.screenRows==1 &&
              wrappedButton.screenRows==2 &&
              wrappedButton.clientHeight-noButtonWrap.clientHeight==38,
            "Screen-off button stays beside two tabs if possible, otherwise wraps cleanly");
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
            "Long monitor names ellipsize instead of enforcing a wider window");
        const auto tiny=calculateWindowContentMinimum(96,180,tabs,2,false,true);
        check(tiny.clientWidth==180 && tiny.screenRows==4,
            "At minimum width the screen-off button wraps after three monitor rows");
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
        // Close (X), minimize-to-tray, and tray-icon toggles use one
        // nonactivating hide operation. SW_HIDE would request activation of
        // Explorer and can cause its Start tooltip to appear unexpectedly.
        check((kHideToTrayFlags&SWP_HIDEWINDOW)!=0 &&
              (kHideToTrayFlags&SWP_NOACTIVATE)!=0,
            "Closing to tray hides without activating another window");
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
        check(resolveStartup(L"normal",L"",true)==L"normal","Normal startup");
        check(resolveStartup(L"minimized",L"",true)==L"minimized","Minimized startup");
        check(resolveStartup(L"tray",L"",true)==L"tray","Tray startup");
        check(resolveStartup(L"tray",L"",false)==L"normal","No invisible app without tray");
        check(resolveStartup(L"tray",L"--normal",true)==L"normal","CLI override");

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
            check(model.preferences().startup==L"tray","Tray mode");
            check(!model.windowPosition().has_value(),"Legacy config without a window position keeps default placement");
            check(!model.windowSize().has_value(),"Legacy config without dimensions keeps automatic first-run sizing");
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
            updated.startup=L"minimized";
            updated.theme=L"system";
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
            check(model.preferences().startup==L"minimized","Saved startup mode");
            check(model.preferences().theme==L"system",
                "System-following theme round-trips through portable config");
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
        fs::remove_all(root,ec);
        std::wcout<<L"All native regressions passed.\n";
        return 0;
    } catch (const std::exception& err) {
        std::cerr<<"Native regressions failed: "<<err.what()<<"\n";
        return 1;
    }
}
