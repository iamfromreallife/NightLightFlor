#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace nightlight {
// Convert a safe, native subset of GitHub-flavoured Markdown to RTF. All
// literal characters are escaped; source HTML is never executed or loaded.
// RGB values are packed as 0xRRGGBB, independent of Win32 COLORREF.
class ReleaseMarkdown {
    using View=std::wstring_view;
    std::vector<unsigned> colors_;
    std::string body_;
    static bool starts(View s,View prefix){return s.substr(0,prefix.size())==prefix;}
    static bool space(wchar_t c){return c==L' '||c==L'\t';}
    static View trim(View s){while(!s.empty()&&space(s.front()))s.remove_prefix(1);while(!s.empty()&&space(s.back()))s.remove_suffix(1);return s;}
    int color(unsigned rgb){auto it=std::find(colors_.begin(),colors_.end(),rgb);if(it!=colors_.end())return int(it-colors_.begin())+1;if(colors_.size()>=128)return 1;colors_.push_back(rgb);return int(colors_.size());}
    void literal(wchar_t c){
        if(c==L'\\'||c==L'{'||c==L'}'){body_+='\\';body_+=char(c);}
        else if(c==L'\t')body_+="\\tab ";
        else if(c>=32&&c<127)body_+=char(c);
        else if(c>=32){auto u=static_cast<uint32_t>(c);auto unit=[&](uint32_t x){body_+="\\u"+std::to_string(x>=32768?int(x)-65536:int(x))+"?";};if(u>65535&&u<=0x10ffff){u-=0x10000;unit(0xd800+(u>>10));unit(0xdc00+(u&1023));}else if(u<=65535)unit(u);}
    }
    void literal(View s){for(auto c:s)literal(c);}
    static int hex(wchar_t c){if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'a'&&c<=L'f')return c-L'a'+10;if(c>=L'A'&&c<=L'F')return c-L'A'+10;return -1;}
    static bool parseColor(View s,unsigned& result){s=trim(s);if(s.empty()||s.front()!=L'#')return false;s.remove_prefix(1);if(s.size()!=3&&s.size()!=6)return false;unsigned v=0;for(auto c:s){int n=hex(c);if(n<0)return false;v=(v<<4)|unsigned(n);}if(s.size()==3)v=((v>>8)&15)*0x110000+((v>>4)&15)*0x1100+(v&15)*0x11;result=v;return true;}
    void inlineText(View s,int depth=0){
        if(depth>=12||s.size()>8192){literal(s);return;}
        for(size_t i=0;i<s.size();){
            View rest=s.substr(i);
            if(rest.front()==L'\\'&&rest.size()>1&&View(L"\\`*_{}[]()#+-.!|<>~").find(rest[1])!=View::npos){literal(rest[1]);i+=2;continue;}
            if(rest.front()==L'`'){
                size_t ticks=1;while(ticks<rest.size()&&rest[ticks]==L'`')++ticks;
                const auto end=rest.find(rest.substr(0,ticks),ticks);
                if(end!=View::npos){auto code=rest.substr(ticks,end-ticks);body_+="{\\f1 ";literal(code);unsigned rgb=0;if(parseColor(code,rgb)){body_+="{\\cf"+std::to_string(color(rgb))+" ";literal(L" \u25a0");body_+='}';}body_+='}';i+=end+ticks;continue;}
            }
            // Links display their label and destination; no remote content or
            // clickable arbitrary protocols is embedded in the RTF.
            if(rest.front()==L'['){auto mid=rest.find(L"](");if(mid!=View::npos){auto end=rest.find(L')',mid+2);if(end!=View::npos){auto url=rest.substr(mid+2,end-mid-2);if(starts(url,L"https://")||starts(url,L"http://")){body_+="{\\cf2\\ul ";inlineText(rest.substr(1,mid-1),depth+1);body_+='}';body_+=" (";literal(url);body_+=')';i+=end+1;continue;}}}}
            struct Mark{View text;const char* rtf;};
            static constexpr Mark marks[]={{L"***","\\b\\i "},{L"___","\\b\\i "},{L"**","\\b "},{L"__","\\b "},{L"~~","\\strike "},{L"*","\\i "},{L"_","\\i "}};
            bool matched=false;
            for(const auto& m:marks){if(!starts(rest,m.text)||rest.size()<=m.text.size()||space(rest[m.text.size()]))continue;if(m.text==L"_"&&i&&((s[i-1]>=L'a'&&s[i-1]<=L'z')||(s[i-1]>=L'A'&&s[i-1]<=L'Z')))continue;auto end=rest.find(m.text,m.text.size());if(end==View::npos||end==m.text.size()||space(rest[end-1]))continue;body_+='{';body_+=m.rtf;inlineText(rest.substr(m.text.size(),end-m.text.size()),depth+1);body_+='}';i+=end+m.text.size();matched=true;break;}
            if(matched)continue;
            // A restricted set of inline HTML tags used in release notes.
            struct Tag{View open,close;const char* rtf;};
            static constexpr Tag tags[]={{L"<b>",L"</b>","\\b "},{L"<strong>",L"</strong>","\\b "},{L"<i>",L"</i>","\\i "},{L"<em>",L"</em>","\\i "},{L"<s>",L"</s>","\\strike "},{L"<del>",L"</del>","\\strike "},{L"<u>",L"</u>","\\ul "},{L"<code>",L"</code>","\\f1 "}};
            for(const auto& t:tags){if(!starts(rest,t.open))continue;auto end=rest.find(t.close,t.open.size());if(end==View::npos)continue;body_+='{';body_+=t.rtf;inlineText(rest.substr(t.open.size(),end-t.open.size()),depth+1);body_+='}';i+=end+t.close.size();matched=true;break;}
            if(matched)continue;
            if(starts(rest,L"<br>")){body_+="\\line ";i+=4;continue;}
            if(starts(rest,L"<br/>")){body_+="\\line ";i+=5;continue;}
            if(starts(rest,L"<br />")){body_+="\\line ";i+=6;continue;}
            if(starts(rest,L"<span style=\"color:")||starts(rest,L"<span style='color:")){
                constexpr size_t prefix=19;auto gt=rest.find(L'>');if(gt!=View::npos&&gt<64&&gt>prefix){auto end=rest.find(L"</span>",gt+1);if(end!=View::npos){auto val=trim(rest.substr(prefix,gt-prefix));if(!val.empty()&&(val.back()==L'\''||val.back()==L'\"'))val.remove_suffix(1);val=trim(val);if(!val.empty()&&val.back()==L';')val.remove_suffix(1);unsigned rgb=0;if(parseColor(val,rgb)){body_+="{\\cf"+std::to_string(color(rgb))+" ";inlineText(rest.substr(gt+1,end-gt-1),depth+1);body_+='}';i+=end+7;continue;}}}
            }
            if(starts(rest,L"&amp;")){literal(L'&');i+=5;continue;}if(starts(rest,L"&lt;")){literal(L'<');i+=4;continue;}if(starts(rest,L"&gt;")){literal(L'>');i+=4;continue;}
            literal(s[i++]);
        }
    }
public:
    std::string render(View source,unsigned foreground,unsigned accent,int fontHalfPoints=20){
        colors_.clear();body_.clear();color(foreground);color(accent);if(colors_.size()==1)colors_.push_back(accent);
        bool fence=false;wchar_t fenceChar=0;size_t fenceCount=0;
        for(size_t pos=0;pos<source.size();){
            auto end=source.find(L'\n',pos);if(end==View::npos)end=source.size();auto line=source.substr(pos,end-pos);if(!line.empty()&&line.back()==L'\r')line.remove_suffix(1);pos=end<source.size()?end+1:end;
            auto clean=trim(line);size_t run=0;if(!clean.empty()&&(clean.front()==L'`'||clean.front()==L'~'))while(run<clean.size()&&clean[run]==clean.front())++run;
            if(run>=3&&(!fence||(clean.front()==fenceChar&&run>=fenceCount&&trim(clean.substr(run)).empty()))){if(fence){fence=false;body_+="\\pard\\sa80\\par\n";}else{fence=true;fenceChar=clean.front();fenceCount=run;}continue;}
            body_+="\\pard\\cf1\\fs"+std::to_string(fontHalfPoints)+"\\sa80 ";
            if(fence){body_+="{\\f1 ";literal(line);body_+="}\\par\n";continue;}
            if(clean.empty()){body_+="\\par\n";continue;}
            size_t h=0;while(h<clean.size()&&h<6&&clean[h]==L'#')++h;
            if(h&&h<clean.size()&&space(clean[h])){body_+="{\\b\\fs"+std::to_string(fontHalfPoints+std::max(2,10-int(h)*2))+" ";inlineText(trim(clean.substr(h)));body_+="}\\par\n";continue;}
            if(clean==L"---"||clean==L"***"||clean==L"___"){body_+="\\brdrb\\brdrs\\brdrw10\\brdrcf2\\par\n";continue;}
            if(starts(clean,L"> ")){body_+="\\li240 {\\i ";inlineText(clean.substr(2));body_+="}\\par\n";continue;}
            size_t indent=0;while(indent<line.size()&&space(line[indent]))++indent;
            if(starts(clean,L"- ")||starts(clean,L"* ")||starts(clean,L"+ ")){body_+="\\li"+std::to_string(300+std::min<size_t>(indent,12)*120)+"\\fi-240 ";clean.remove_prefix(2);if(starts(clean,L"[x] ")||starts(clean,L"[X] ")){literal(L'\u2611');clean.remove_prefix(4);}else if(starts(clean,L"[ ] ")){literal(L'\u2610');clean.remove_prefix(4);}else literal(L'\u2022');body_+="\\tab ";}
            else {size_t digits=0;while(digits<clean.size()&&clean[digits]>=L'0'&&clean[digits]<=L'9')++digits;if(digits&&digits+1<clean.size()&&(clean[digits]==L'.'||clean[digits]==L')')&&space(clean[digits+1]))body_+="\\li"+std::to_string(300+std::min<size_t>(indent,12)*120)+"\\fi-240 ";}
            inlineText(clean);body_+="\\par\n";
        }
        std::string header="{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0 Segoe UI;}{\\f1 Consolas;}}{\\colortbl;";
        for(unsigned c:colors_)header+="\\red"+std::to_string((c>>16)&255)+"\\green"+std::to_string((c>>8)&255)+"\\blue"+std::to_string(c&255)+";";
        return header+"}\\viewkind4\\f0\\cf1\\fs"+std::to_string(fontHalfPoints)+" "+body_+"}";
    }
};
}
