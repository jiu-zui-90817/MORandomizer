/**
 * =========================================================================================
 * 项目名称: MORandomizer (心灵终结/红警2 武器随机化引擎)
 * 核心功能: 动态劫持游戏内存，实现全局/局部的单位武器随机化，支持跨物种混沌洗牌。
 * * 按键说明:
 * - [W] 键: 全局同阶级洗牌（步兵抽步兵，坦克抽坦克）。
 * - [C] 键: 全局大乱炖（全军种跨物种抽武器）。
 * - [S] 键: 局部精准洗牌（仅针对当前鼠标框选的单位进行武器更换）。
 * - [E] 键: 状态开关（切换 S 键是抽取同级武器，还是跨物种大杂烩武器）。
 * - [R] 键: 一键恢复原厂武器图纸。
 * * 核心防御机制:
 * 1. 精英打包: 普通武器与精英(三星)武器成对发放，防止单位升星后哑火。
 * 2. 平民隔离: 严格校验原配武器 (Backup.Pri)，绝不给路牌、树木、平民发放武器。
 * 3. 假武器过滤: 引擎级拦截近战、无弹道、无弹头及名字带 Dummy/Fake/AI 的逻辑武器。
 * =========================================================================================
 */

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include <string>
#include <stdio.h>
#include <cctype>
#include <set>

#include <Syringe.h>
#include <YRpp.h>
#include <InfantryTypeClass.h>
#include <UnitTypeClass.h>
#include <BuildingTypeClass.h> 
#include <AircraftTypeClass.h> 
#include <WeaponTypeClass.h>
#include <WarheadTypeClass.h>

#include <InfantryClass.h>
#include <UnitClass.h>
#include <AircraftClass.h>
#include <BuildingClass.h>

 // ==========================================
 // 全局配置与状态变量
 // ==========================================
bool enableBeeps = true;        // 是否开启提示音
bool enableDebugLog = false;    // 是否输出黑匣子日志
std::string logFilePath = "";   // 日志文件路径

// 组合键要求设定 (Ctrl / Shift / Alt)
bool requireCtrl = true;
bool requireShift = false;
bool requireAlt = false;

// 热键映射
int vkRandomWeapons = 'W';      // 全局：同级洗牌
int vkChaosMode = 'C';          // 全局：跨物种大乱炖
int vkSelectedRandom = 'S';     // 局部：针对选中单位发牌
int vkToggleChaosSelect = 'E';  // 开关：控制S键是否跨物种
int vkRestore = 'R';            // 恢复：还原原厂图纸

// 兵种参与随机化开关
bool randInfantry = true;
bool randUnits = true;
bool randAircraft = true;
bool randBuildings = true;
bool includeBldInChaos = false; // 建筑武器是否纳入混沌大杂烩池

bool isChaosToggleOn = false;   // 记录 E 键当前的开关状态

std::vector<std::string> WhiteList;             // 免受随机化影响的单位白名单
std::vector<std::string> SuperWeaponBlacklist;  // 导致崩溃的危险武器词根黑名单

// ==========================================
// 核心数据结构
// ==========================================

// 记录单位原厂的武器图纸配置
struct WeaponSet {
    WeaponTypeClass* Pri;   WeaponTypeClass* Sec;    // 普通主/副武器
    WeaponTypeClass* EPri;  WeaponTypeClass* ESec;   // 精英主/副武器
    WeaponTypeClass* Occ;   WeaponTypeClass* EOcc;   // 驻军(进屋)武器
};

// 绑定普通武器与精英武器，防止升级后武器丢失
struct WeaponPair {
    WeaponTypeClass* Normal;
    WeaponTypeClass* Elite;
};

// 原厂武器快照备份库
std::vector<WeaponSet> BackupInfWeapons;
std::vector<WeaponSet> BackupUnitWeapons;
std::vector<WeaponSet> BackupAircraftWeapons;
std::vector<WeaponSet> BackupBuildingWeapons;
bool isBackedUp = false; // 标记快照是否已初始化

// 同阶级安全武器池
std::vector<WeaponPair> SafeInfWeapons;
std::vector<WeaponPair> SafeUnitWeapons;
std::vector<WeaponPair> SafeAircraftWeapons;
std::vector<WeaponPair> SafeBuildingWeapons;
std::vector<WeaponPair> SafeOccupyWeapons;

// 跨物种混沌大乱炖武器池
std::vector<WeaponPair> GlobalChaosPool;

// ==========================================
// 辅助与工具函数
// ==========================================

// 带缓冲刷新的线程安全日志输出（防断电/崩溃丢失）
void LogDebug(const char* format, ...) {
    if (!enableDebugLog) return;
    FILE* fp = fopen(logFilePath.c_str(), "a");
    if (fp) {
        time_t now = time(NULL);
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localtime(&now));
        fprintf(fp, "[%s] ", timeStr);
        va_list args;
        va_start(args, format);
        vfprintf(fp, format, args);
        va_end(args);
        fprintf(fp, "\n");
        fflush(fp);
        fclose(fp);
    }
}

// 打印指定武器池的内容（使用 set 进行去重展示）
void DumpPoolToLog(const char* poolName, const std::vector<WeaponPair>& pool) {
    if (!enableDebugLog) return;
    std::set<std::string> uniqueWeapons;
    for (auto wp : pool) {
        if (wp.Normal && wp.Normal->ID) uniqueWeapons.insert(wp.Normal->ID);
    }
    std::string dumpStr = "";
    for (const auto& name : uniqueWeapons) dumpStr += name + ", ";
    LogDebug("【%s】(共 %d 套): %s", poolName, uniqueWeapons.size(), dumpStr.c_str());
}

// 字符串去除首尾空白
std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

// 解析 INI 文件中的列表段落
void ParseListBuffer(char* buffer, std::vector<std::string>& listOut, const char* listName) {
    char* p = buffer;
    std::string dumpStr = "";
    while (*p) {
        std::string line(p);
        size_t commentPos = line.find(';'); // 忽略注释
        if (commentPos != std::string::npos) line = line.substr(0, commentPos);
        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) line = line.substr(0, eqPos);
        line = Trim(line);
        if (!line.empty()) {
            listOut.push_back(line);
            dumpStr += line + " | ";
        }
        p += strlen(p) + 1;
    }
    LogDebug("[系统] %s 成功载入 %d 项: %s", listName, listOut.size(), dumpStr.c_str());
}

void PlayBeep(DWORD freq, DWORD duration) {
    if (enableBeeps) Beep(freq, duration);
}

// 字符串后缀不区分大小写匹配
bool EndsWithCaseInsensitive(const char* str, const char* suffix) {
    if (!str || !suffix) return false;
    size_t strLen = strlen(str);
    size_t suffixLen = strlen(suffix);
    if (strLen < suffixLen) return false;
    return _stricmp(str + strLen - suffixLen, suffix) == 0;
}

// 读取 MORandomizer.ini 外部配置
void LoadConfiguration() {
    char iniPath[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, iniPath);
    strcat(iniPath, "\\MORandomizer.ini");

    logFilePath = std::string(iniPath);
    size_t pos = logFilePath.find_last_of("\\/");
    logFilePath = logFilePath.substr(0, pos) + "\\MORandomizer_Debug.log";

    char buf[32];
    GetPrivateProfileStringA("Settings", "EnableBeep", "yes", buf, sizeof(buf), iniPath);
    enableBeeps = (_stricmp(Trim(buf).c_str(), "no") != 0);
    GetPrivateProfileStringA("Settings", "EnableDebugLog", "no", buf, sizeof(buf), iniPath);
    enableDebugLog = (_stricmp(Trim(buf).c_str(), "yes") == 0);

    GetPrivateProfileStringA("Settings", "RequireCtrl", "yes", buf, sizeof(buf), iniPath);
    requireCtrl = (_stricmp(Trim(buf).c_str(), "yes") == 0);
    GetPrivateProfileStringA("Settings", "RequireShift", "no", buf, sizeof(buf), iniPath);
    requireShift = (_stricmp(Trim(buf).c_str(), "yes") == 0);
    GetPrivateProfileStringA("Settings", "RequireAlt", "no", buf, sizeof(buf), iniPath);
    requireAlt = (_stricmp(Trim(buf).c_str(), "yes") == 0);

    GetPrivateProfileStringA("Settings", "KeyRandomWeapons", "W", buf, sizeof(buf), iniPath);
    if (!Trim(buf).empty()) vkRandomWeapons = toupper(Trim(buf)[0]);
    GetPrivateProfileStringA("Settings", "KeyChaosMode", "C", buf, sizeof(buf), iniPath);
    if (!Trim(buf).empty()) vkChaosMode = toupper(Trim(buf)[0]);
    GetPrivateProfileStringA("Settings", "KeySelectedRandom", "S", buf, sizeof(buf), iniPath);
    if (!Trim(buf).empty()) vkSelectedRandom = toupper(Trim(buf)[0]);
    GetPrivateProfileStringA("Settings", "KeyToggleChaos", "E", buf, sizeof(buf), iniPath);
    if (!Trim(buf).empty()) vkToggleChaosSelect = toupper(Trim(buf)[0]);
    GetPrivateProfileStringA("Settings", "KeyRestore", "R", buf, sizeof(buf), iniPath);
    if (!Trim(buf).empty()) vkRestore = toupper(Trim(buf)[0]);

    GetPrivateProfileStringA("Settings", "RandomizeInfantry", "yes", buf, sizeof(buf), iniPath);
    randInfantry = (_stricmp(Trim(buf).c_str(), "no") != 0);
    GetPrivateProfileStringA("Settings", "RandomizeUnits", "yes", buf, sizeof(buf), iniPath);
    randUnits = (_stricmp(Trim(buf).c_str(), "no") != 0);
    GetPrivateProfileStringA("Settings", "RandomizeAircraft", "yes", buf, sizeof(buf), iniPath);
    randAircraft = (_stricmp(Trim(buf).c_str(), "no") != 0);
    GetPrivateProfileStringA("Settings", "RandomizeBuildings", "yes", buf, sizeof(buf), iniPath);
    randBuildings = (_stricmp(Trim(buf).c_str(), "no") != 0);

    GetPrivateProfileStringA("Settings", "IncludeBuildingsInChaos", "no", buf, sizeof(buf), iniPath);
    includeBldInChaos = (_stricmp(Trim(buf).c_str(), "yes") == 0);

    if (enableDebugLog) {
        FILE* fp = fopen(logFilePath.c_str(), "w");
        if (fp) {
            fprintf(fp, "========== MORandomizer 引擎启动 ==========\n");
            fclose(fp);
        }
    }

    char buffer[4096];
    memset(buffer, 0, sizeof(buffer));
    GetPrivateProfileSectionA("Whitelist", buffer, sizeof(buffer), iniPath);
    ParseListBuffer(buffer, WhiteList, "单位白名单");

    memset(buffer, 0, sizeof(buffer));
    GetPrivateProfileSectionA("Blacklist", buffer, sizeof(buffer), iniPath);
    ParseListBuffer(buffer, SuperWeaponBlacklist, "武器黑名单");
}

// 核心安检拦截器：判定获取的武器是否会导致游戏逻辑死锁或崩溃
bool IsLegalWeapon(WeaponTypeClass* pWeapon) {
    if (!pWeapon || !pWeapon->ID) return false;

    // 拦截无实体弹道、无伤害弹头及特种短射程（如特种兵C4）武器
    if (!pWeapon->Projectile || !pWeapon->Warhead || pWeapon->Range <= 2.5) return false;
    if (EndsWithCaseInsensitive(pWeapon->ID, "AI")) return false; // 拦截 AI 专属控制武器

    std::string wUpper = pWeapon->ID;
    for (auto& c : wUpper) c = toupper((unsigned char)c);

    // 拦截硬编码的底层假武器特征码
    if (wUpper.find("DUMMY") != std::string::npos || wUpper.find("FAKE") != std::string::npos ||
        wUpper.find("NOTA") != std::string::npos || wUpper.find("SUPPORT") != std::string::npos ||
        wUpper.find("START") != std::string::npos || wUpper.find("PATH") != std::string::npos ||
        wUpper.find("MINDCONTROL") != std::string::npos || wUpper.find("MINE") != std::string::npos ||
        wUpper.find("BOMB") != std::string::npos) return false;

    // 根据 INI 黑名单进行模糊特征查杀 (如 Strike 空袭, Spawn 生成器等)
    for (const auto& bw : SuperWeaponBlacklist) {
        std::string bwUpper = bw;
        for (auto& c : bwUpper) c = toupper((unsigned char)c);
        if (wUpper.find(bwUpper) != std::string::npos) return false;
    }
    return true;
}

// 白名单豁免检查
bool IsWhitelisted(const char* id) {
    if (!id) return false;
    for (const auto& w : WhiteList) {
        if (_stricmp(id, w.c_str()) == 0) return true;
    }
    return false;
}

// 将合法武器入库，组装为成套的盲盒
void TryAddPair(std::vector<WeaponPair>& pool, WeaponTypeClass* norm, WeaponTypeClass* elite) {
    if (IsLegalWeapon(norm)) {
        WeaponPair wp;
        wp.Normal = norm;
        wp.Elite = (elite && IsLegalWeapon(elite)) ? elite : norm; // 若无精英版，则用普通版兜底
        pool.push_back(wp);
    }
}

// ==========================================
// 引擎主执行线程
// ==========================================
DWORD WINAPI GodHandThread(LPVOID lpParam)
{
    srand((unsigned)time(NULL));
    LoadConfiguration();

    while (true)
    {
        // 1. 验证组合键状态
        bool modCtrl = requireCtrl ? (GetAsyncKeyState(VK_CONTROL) & 0x8000) : true;
        bool modShift = requireShift ? (GetAsyncKeyState(VK_SHIFT) & 0x8000) : true;
        bool modAlt = requireAlt ? (GetAsyncKeyState(VK_MENU) & 0x8000) : true;
        bool modsOk = modCtrl && modShift && modAlt;

        bool pressW = modsOk && (GetAsyncKeyState(vkRandomWeapons) & 0x8000);
        bool pressC = modsOk && (GetAsyncKeyState(vkChaosMode) & 0x8000);
        bool pressS = modsOk && (GetAsyncKeyState(vkSelectedRandom) & 0x8000);
        bool pressE = modsOk && (GetAsyncKeyState(vkToggleChaosSelect) & 0x8000);
        bool pressR = modsOk && (GetAsyncKeyState(vkRestore) & 0x8000);

        // 2. 初始化快照（在任意功能键首次按下时触发）
        if (!isBackedUp && (pressW || pressC || pressS || pressE || pressR)) {
            if (InfantryTypeClass::Array.Count > 0) {

                // --- 备份图纸并组装各军种安全池 ---
                for (int i = 0; i < InfantryTypeClass::Array.Count; i++) {
                    InfantryTypeClass* pInf = InfantryTypeClass::Array.Items[i];
                    WeaponSet ws = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
                    if (pInf) {
                        ws.Pri = pInf->Weapon[0].WeaponType; ws.Sec = pInf->Weapon[1].WeaponType;
                        ws.EPri = pInf->EliteWeapon[0].WeaponType; ws.ESec = pInf->EliteWeapon[1].WeaponType;
                        ws.Occ = pInf->OccupyWeapon.WeaponType; ws.EOcc = pInf->EliteOccupyWeapon.WeaponType;
                        TryAddPair(SafeInfWeapons, ws.Pri, ws.EPri); TryAddPair(SafeInfWeapons, ws.Sec, ws.ESec); TryAddPair(SafeOccupyWeapons, ws.Occ, ws.EOcc);
                    }
                    BackupInfWeapons.push_back(ws);
                }
                for (int i = 0; i < UnitTypeClass::Array.Count; i++) {
                    UnitTypeClass* pUnit = UnitTypeClass::Array.Items[i];
                    WeaponSet ws = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
                    if (pUnit) {
                        ws.Pri = pUnit->Weapon[0].WeaponType; ws.Sec = pUnit->Weapon[1].WeaponType;
                        ws.EPri = pUnit->EliteWeapon[0].WeaponType; ws.ESec = pUnit->EliteWeapon[1].WeaponType;
                        TryAddPair(SafeUnitWeapons, ws.Pri, ws.EPri); TryAddPair(SafeUnitWeapons, ws.Sec, ws.ESec);
                    }
                    BackupUnitWeapons.push_back(ws);
                }
                for (int i = 0; i < AircraftTypeClass::Array.Count; i++) {
                    AircraftTypeClass* pAir = AircraftTypeClass::Array.Items[i];
                    WeaponSet ws = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
                    if (pAir) {
                        ws.Pri = pAir->Weapon[0].WeaponType; ws.Sec = pAir->Weapon[1].WeaponType;
                        ws.EPri = pAir->EliteWeapon[0].WeaponType; ws.ESec = pAir->EliteWeapon[1].WeaponType;
                        TryAddPair(SafeAircraftWeapons, ws.Pri, ws.EPri); TryAddPair(SafeAircraftWeapons, ws.Sec, ws.ESec);
                    }
                    BackupAircraftWeapons.push_back(ws);
                }
                for (int i = 0; i < BuildingTypeClass::Array.Count; i++) {
                    BuildingTypeClass* pBld = BuildingTypeClass::Array.Items[i];
                    WeaponSet ws = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
                    if (pBld) {
                        ws.Pri = pBld->Weapon[0].WeaponType; ws.Sec = pBld->Weapon[1].WeaponType;
                        ws.EPri = pBld->EliteWeapon[0].WeaponType; ws.ESec = pBld->EliteWeapon[1].WeaponType;
                        TryAddPair(SafeBuildingWeapons, ws.Pri, ws.EPri); TryAddPair(SafeBuildingWeapons, ws.Sec, ws.ESec);
                    }
                    BackupBuildingWeapons.push_back(ws);
                }

                // --- 汇聚为全局大乱炖池 ---
                GlobalChaosPool.insert(GlobalChaosPool.end(), SafeInfWeapons.begin(), SafeInfWeapons.end());
                GlobalChaosPool.insert(GlobalChaosPool.end(), SafeUnitWeapons.begin(), SafeUnitWeapons.end());
                GlobalChaosPool.insert(GlobalChaosPool.end(), SafeAircraftWeapons.begin(), SafeAircraftWeapons.end());
                GlobalChaosPool.insert(GlobalChaosPool.end(), SafeBuildingWeapons.begin(), SafeBuildingWeapons.end());

                isBackedUp = true;
                LogDebug("<<< 快照收集完成！");
                DumpPoolToLog("步兵套装池", SafeInfWeapons);
                DumpPoolToLog("驻军套装池", SafeOccupyWeapons);
                DumpPoolToLog("载具套装池", SafeUnitWeapons);
                DumpPoolToLog("空军套装池", SafeAircraftWeapons);
                DumpPoolToLog("建筑套装池", SafeBuildingWeapons);
                DumpPoolToLog("终极大乱炖池", GlobalChaosPool);
                PlayBeep(800, 300); Sleep(200);
            }
        }

        if (isBackedUp)
        {
            // ================== E键：状态机，控制S键抽取池的模式 ==================
            if (pressE)
            {
                isChaosToggleOn = !isChaosToggleOn;
                if (isChaosToggleOn) {
                    PlayBeep(2000, 100); PlayBeep(2500, 100);
                    LogDebug(">>> [状态] E键已【开启】: S键现为【跨物种】洗牌 <<<");
                }
                else {
                    PlayBeep(2500, 100); PlayBeep(2000, 100);
                    LogDebug(">>> [状态] E键已【关闭】: S键现为【同阶级】洗牌 <<<");
                }
                Sleep(300); // 延迟防抖
            }

            // ================== S键：局部精准变异 (仅对框选的单位发牌) ==================
            if (pressS)
            {
                LogDebug(">>> 局部洗牌开始 (%c键) <<<", vkSelectedRandom);
                PlayBeep(1500, 100);

                std::set<InfantryTypeClass*> selectedInfTypes;
                std::set<UnitTypeClass*> selectedUnitTypes;
                std::set<AircraftTypeClass*> selectedAirTypes;
                std::set<BuildingTypeClass*> selectedBldTypes;

                // 1. 根据底层 IsSelected 状态，捕获所有被绿框选中的实体类型 (通过 Set 自动去重)
                if (randInfantry && SafeInfWeapons.size() > 0) {
                    for (int i = 0; i < InfantryClass::Array.Count; i++) {
                        InfantryClass* pObj = InfantryClass::Array.Items[i];
                        if (pObj && pObj->IsSelected && pObj->Type && !IsWhitelisted(pObj->Type->ID)) selectedInfTypes.insert(pObj->Type);
                    }
                }
                if (randUnits && SafeUnitWeapons.size() > 0) {
                    for (int i = 0; i < UnitClass::Array.Count; i++) {
                        UnitClass* pObj = UnitClass::Array.Items[i];
                        if (pObj && pObj->IsSelected && pObj->Type && !IsWhitelisted(pObj->Type->ID)) selectedUnitTypes.insert(pObj->Type);
                    }
                }
                if (randAircraft && SafeAircraftWeapons.size() > 0) {
                    for (int i = 0; i < AircraftClass::Array.Count; i++) {
                        AircraftClass* pObj = AircraftClass::Array.Items[i];
                        if (pObj && pObj->IsSelected && pObj->Type && !IsWhitelisted(pObj->Type->ID)) selectedAirTypes.insert(pObj->Type);
                    }
                }
                if (randBuildings && SafeBuildingWeapons.size() > 0) {
                    for (int i = 0; i < BuildingClass::Array.Count; i++) {
                        BuildingClass* pObj = BuildingClass::Array.Items[i];
                        if (pObj && pObj->IsSelected && pObj->Type && !IsWhitelisted(pObj->Type->ID)) selectedBldTypes.insert(pObj->Type);
                    }
                }

                if (selectedInfTypes.empty() && selectedUnitTypes.empty() && selectedAirTypes.empty() && selectedBldTypes.empty()) {
                    LogDebug("【提示】未检测到选中的单位！");
                }

                // 2. 依次遍历选中的实体，并发放对应的盲盒武器
                // 保安大门：仅对原厂图纸具有该武器槽位的实体发牌，防止未武装平民、树木开火暴走
                for (auto pInfType : selectedInfTypes) {
                    int typeIdx = -1;
                    for (int idx = 0; idx < InfantryTypeClass::Array.Count; ++idx) {
                        if (InfantryTypeClass::Array.Items[idx] == pInfType) { typeIdx = idx; break; }
                    }
                    if (typeIdx != -1 && typeIdx < (int)BackupInfWeapons.size()) {
                        if (BackupInfWeapons[typeIdx].Pri) {
                            WeaponPair wp1 = isChaosToggleOn ? GlobalChaosPool[rand() % GlobalChaosPool.size()] : SafeInfWeapons[rand() % SafeInfWeapons.size()];
                            pInfType->Weapon[0].WeaponType = wp1.Normal; pInfType->EliteWeapon[0].WeaponType = wp1.Elite;
                            LogDebug("【局部】步兵 [%s] 主武 -> 普通[%s] | 精英[%s]", pInfType->ID, wp1.Normal->ID, wp1.Elite->ID);
                        }
                        if (BackupInfWeapons[typeIdx].Sec) {
                            WeaponPair wp2 = isChaosToggleOn ? GlobalChaosPool[rand() % GlobalChaosPool.size()] : SafeInfWeapons[rand() % SafeInfWeapons.size()];
                            pInfType->Weapon[1].WeaponType = wp2.Normal; pInfType->EliteWeapon[1].WeaponType = wp2.Elite;
                            LogDebug("【局部】步兵 [%s] 副武 -> 普通[%s] | 精英[%s]", pInfType->ID, wp2.Normal->ID, wp2.Elite->ID);
                        }
                        if (BackupInfWeapons[typeIdx].Occ && SafeOccupyWeapons.size() > 0) {
                            WeaponPair wp3 = isChaosToggleOn ? GlobalChaosPool[rand() % GlobalChaosPool.size()] : SafeOccupyWeapons[rand() % SafeOccupyWeapons.size()];
                            pInfType->OccupyWeapon.WeaponType = wp3.Normal; pInfType->EliteOccupyWeapon.WeaponType = wp3.Elite;
                            LogDebug("【局部】步兵 [%s] 驻军 -> 普通[%s] | 精英[%s]", pInfType->ID, wp3.Normal->ID, wp3.Elite->ID);
                        }
                    }
                }

                for (auto pUnitType : selectedUnitTypes) {
                    int typeIdx = -1;
                    for (int idx = 0; idx < UnitTypeClass::Array.Count; ++idx) {
                        if (UnitTypeClass::Array.Items[idx] == pUnitType) { typeIdx = idx; break; }
                    }
                    if (typeIdx != -1 && typeIdx < (int)BackupUnitWeapons.size()) {
                        if (BackupUnitWeapons[typeIdx].Pri) {
                            WeaponPair wp1 = isChaosToggleOn ? GlobalChaosPool[rand() % GlobalChaosPool.size()] : SafeUnitWeapons[rand() % SafeUnitWeapons.size()];
                            pUnitType->Weapon[0].WeaponType = wp1.Normal; pUnitType->EliteWeapon[0].WeaponType = wp1.Elite;
                            LogDebug("【局部】载具 [%s] 主武 -> 普通[%s] | 精英[%s]", pUnitType->ID, wp1.Normal->ID, wp1.Elite->ID);
                        }
                        if (BackupUnitWeapons[typeIdx].Sec) {
                            WeaponPair wp2 = isChaosToggleOn ? GlobalChaosPool[rand() % GlobalChaosPool.size()] : SafeUnitWeapons[rand() % SafeUnitWeapons.size()];
                            pUnitType->Weapon[1].WeaponType = wp2.Normal; pUnitType->EliteWeapon[1].WeaponType = wp2.Elite;
                            LogDebug("【局部】载具 [%s] 副武 -> 普通[%s] | 精英[%s]", pUnitType->ID, wp2.Normal->ID, wp2.Elite->ID);
                        }
                    }
                }

                for (auto pAirType : selectedAirTypes) {
                    int typeIdx = -1;
                    for (int idx = 0; idx < AircraftTypeClass::Array.Count; ++idx) {
                        if (AircraftTypeClass::Array.Items[idx] == pAirType) { typeIdx = idx; break; }
                    }
                    if (typeIdx != -1 && typeIdx < (int)BackupAircraftWeapons.size()) {
                        if (BackupAircraftWeapons[typeIdx].Pri) {
                            WeaponPair wp1 = isChaosToggleOn ? GlobalChaosPool[rand() % GlobalChaosPool.size()] : SafeAircraftWeapons[rand() % SafeAircraftWeapons.size()];
                            pAirType->Weapon[0].WeaponType = wp1.Normal; pAirType->EliteWeapon[0].WeaponType = wp1.Elite;
                            LogDebug("【局部】空军 [%s] 主武 -> 普通[%s] | 精英[%s]", pAirType->ID, wp1.Normal->ID, wp1.Elite->ID);
                        }
                        if (BackupAircraftWeapons[typeIdx].Sec) {
                            WeaponPair wp2 = isChaosToggleOn ? GlobalChaosPool[rand() % GlobalChaosPool.size()] : SafeAircraftWeapons[rand() % SafeAircraftWeapons.size()];
                            pAirType->Weapon[1].WeaponType = wp2.Normal; pAirType->EliteWeapon[1].WeaponType = wp2.Elite;
                            LogDebug("【局部】空军 [%s] 副武 -> 普通[%s] | 精英[%s]", pAirType->ID, wp2.Normal->ID, wp2.Elite->ID);
                        }
                    }
                }

                for (auto pBldType : selectedBldTypes) {
                    int typeIdx = -1;
                    for (int idx = 0; idx < BuildingTypeClass::Array.Count; ++idx) {
                        if (BuildingTypeClass::Array.Items[idx] == pBldType) { typeIdx = idx; break; }
                    }
                    if (typeIdx != -1 && typeIdx < (int)BackupBuildingWeapons.size()) {
                        if (BackupBuildingWeapons[typeIdx].Pri) {
                            WeaponPair wp1;
                            if (isChaosToggleOn && includeBldInChaos) wp1 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                            else wp1 = SafeBuildingWeapons[rand() % SafeBuildingWeapons.size()];
                            pBldType->Weapon[0].WeaponType = wp1.Normal; pBldType->EliteWeapon[0].WeaponType = wp1.Elite;
                            LogDebug("【局部】建筑 [%s] 主武 -> 普通[%s] | 精英[%s]", pBldType->ID, wp1.Normal->ID, wp1.Elite->ID);
                        }
                        if (BackupBuildingWeapons[typeIdx].Sec) {
                            WeaponPair wp2;
                            if (isChaosToggleOn && includeBldInChaos) wp2 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                            else wp2 = SafeBuildingWeapons[rand() % SafeBuildingWeapons.size()];
                            pBldType->Weapon[1].WeaponType = wp2.Normal; pBldType->EliteWeapon[1].WeaponType = wp2.Elite;
                            LogDebug("【局部】建筑 [%s] 副武 -> 普通[%s] | 精英[%s]", pBldType->ID, wp2.Normal->ID, wp2.Elite->ID);
                        }
                    }
                }
                LogDebug("<<< 局部洗牌完成 <<<");
                Sleep(300);
            }

            // ================== W键：全局同阶级随机发牌 ==================
            if (pressW)
            {
                LogDebug(">>> 全局同级洗牌开始 (%c键) <<<", vkRandomWeapons);
                PlayBeep(2000, 100);

                if (randInfantry && SafeInfWeapons.size() > 0) {
                    for (int i = 0; i < InfantryTypeClass::Array.Count; i++) {
                        InfantryTypeClass* pInf = InfantryTypeClass::Array.Items[i];
                        if (pInf && !IsWhitelisted(pInf->ID) && i < (int)BackupInfWeapons.size()) {
                            if (BackupInfWeapons[i].Pri) {
                                WeaponPair wp1 = SafeInfWeapons[rand() % SafeInfWeapons.size()];
                                pInf->Weapon[0].WeaponType = wp1.Normal; pInf->EliteWeapon[0].WeaponType = wp1.Elite;
                                LogDebug("【全局】步兵 [%s] 主武 -> 普通[%s] | 精英[%s]", pInf->ID, wp1.Normal->ID, wp1.Elite->ID);
                            }
                            if (BackupInfWeapons[i].Sec) {
                                WeaponPair wp2 = SafeInfWeapons[rand() % SafeInfWeapons.size()];
                                pInf->Weapon[1].WeaponType = wp2.Normal; pInf->EliteWeapon[1].WeaponType = wp2.Elite;
                                LogDebug("【全局】步兵 [%s] 副武 -> 普通[%s] | 精英[%s]", pInf->ID, wp2.Normal->ID, wp2.Elite->ID);
                            }
                            if (BackupInfWeapons[i].Occ && SafeOccupyWeapons.size() > 0) {
                                WeaponPair wp3 = SafeOccupyWeapons[rand() % SafeOccupyWeapons.size()];
                                pInf->OccupyWeapon.WeaponType = wp3.Normal; pInf->EliteOccupyWeapon.WeaponType = wp3.Elite;
                                LogDebug("【全局】步兵 [%s] 驻军 -> 普通[%s] | 精英[%s]", pInf->ID, wp3.Normal->ID, wp3.Elite->ID);
                            }
                        }
                    }
                }
                if (randUnits && SafeUnitWeapons.size() > 0) {
                    for (int i = 0; i < UnitTypeClass::Array.Count; i++) {
                        UnitTypeClass* pUnit = UnitTypeClass::Array.Items[i];
                        if (pUnit && !IsWhitelisted(pUnit->ID) && i < (int)BackupUnitWeapons.size()) {
                            if (BackupUnitWeapons[i].Pri) {
                                WeaponPair wp1 = SafeUnitWeapons[rand() % SafeUnitWeapons.size()];
                                pUnit->Weapon[0].WeaponType = wp1.Normal; pUnit->EliteWeapon[0].WeaponType = wp1.Elite;
                                LogDebug("【全局】载具 [%s] 主武 -> 普通[%s] | 精英[%s]", pUnit->ID, wp1.Normal->ID, wp1.Elite->ID);
                            }
                            if (BackupUnitWeapons[i].Sec) {
                                WeaponPair wp2 = SafeUnitWeapons[rand() % SafeUnitWeapons.size()];
                                pUnit->Weapon[1].WeaponType = wp2.Normal; pUnit->EliteWeapon[1].WeaponType = wp2.Elite;
                                LogDebug("【全局】载具 [%s] 副武 -> 普通[%s] | 精英[%s]", pUnit->ID, wp2.Normal->ID, wp2.Elite->ID);
                            }
                        }
                    }
                }
                if (randAircraft && SafeAircraftWeapons.size() > 0) {
                    for (int i = 0; i < AircraftTypeClass::Array.Count; i++) {
                        AircraftTypeClass* pAir = AircraftTypeClass::Array.Items[i];
                        if (pAir && !IsWhitelisted(pAir->ID) && i < (int)BackupAircraftWeapons.size()) {
                            if (BackupAircraftWeapons[i].Pri) {
                                WeaponPair wp1 = SafeAircraftWeapons[rand() % SafeAircraftWeapons.size()];
                                pAir->Weapon[0].WeaponType = wp1.Normal; pAir->EliteWeapon[0].WeaponType = wp1.Elite;
                                LogDebug("【全局】空军 [%s] 主武 -> 普通[%s] | 精英[%s]", pAir->ID, wp1.Normal->ID, wp1.Elite->ID);
                            }
                            if (BackupAircraftWeapons[i].Sec) {
                                WeaponPair wp2 = SafeAircraftWeapons[rand() % SafeAircraftWeapons.size()];
                                pAir->Weapon[1].WeaponType = wp2.Normal; pAir->EliteWeapon[1].WeaponType = wp2.Elite;
                                LogDebug("【全局】空军 [%s] 副武 -> 普通[%s] | 精英[%s]", pAir->ID, wp2.Normal->ID, wp2.Elite->ID);
                            }
                        }
                    }
                }
                if (randBuildings && SafeBuildingWeapons.size() > 0) {
                    for (int i = 0; i < BuildingTypeClass::Array.Count; i++) {
                        BuildingTypeClass* pBld = BuildingTypeClass::Array.Items[i];
                        if (pBld && !IsWhitelisted(pBld->ID) && i < (int)BackupBuildingWeapons.size()) {
                            if (BackupBuildingWeapons[i].Pri) {
                                WeaponPair wp1 = SafeBuildingWeapons[rand() % SafeBuildingWeapons.size()];
                                pBld->Weapon[0].WeaponType = wp1.Normal; pBld->EliteWeapon[0].WeaponType = wp1.Elite;
                                LogDebug("【全局】建筑 [%s] 主武 -> 普通[%s] | 精英[%s]", pBld->ID, wp1.Normal->ID, wp1.Elite->ID);
                            }
                            if (BackupBuildingWeapons[i].Sec) {
                                WeaponPair wp2 = SafeBuildingWeapons[rand() % SafeBuildingWeapons.size()];
                                pBld->Weapon[1].WeaponType = wp2.Normal; pBld->EliteWeapon[1].WeaponType = wp2.Elite;
                                LogDebug("【全局】建筑 [%s] 副武 -> 普通[%s] | 精英[%s]", pBld->ID, wp2.Normal->ID, wp2.Elite->ID);
                            }
                        }
                    }
                }

                LogDebug("<<< 全局同级洗牌完成 <<<");
                Sleep(300);
            }

            // ================== C键：全局跨物种大乱炖 ==================
            if (pressC)
            {
                LogDebug(">>> 全局大乱炖开始 (%c键) <<<", vkChaosMode);
                PlayBeep(2500, 150);
                if (GlobalChaosPool.size() > 0) {
                    if (randInfantry) {
                        for (int i = 0; i < InfantryTypeClass::Array.Count; i++) {
                            InfantryTypeClass* pInf = InfantryTypeClass::Array.Items[i];
                            if (pInf && !IsWhitelisted(pInf->ID) && i < (int)BackupInfWeapons.size()) {
                                if (BackupInfWeapons[i].Pri) {
                                    WeaponPair wp1 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    pInf->Weapon[0].WeaponType = wp1.Normal; pInf->EliteWeapon[0].WeaponType = wp1.Elite;
                                    LogDebug("【全局混沌】步兵 [%s] 主武 -> 普通[%s] | 精英[%s]", pInf->ID, wp1.Normal->ID, wp1.Elite->ID);
                                }
                                if (BackupInfWeapons[i].Sec) {
                                    WeaponPair wp2 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    pInf->Weapon[1].WeaponType = wp2.Normal; pInf->EliteWeapon[1].WeaponType = wp2.Elite;
                                    LogDebug("【全局混沌】步兵 [%s] 副武 -> 普通[%s] | 精英[%s]", pInf->ID, wp2.Normal->ID, wp2.Elite->ID);
                                }
                                if (BackupInfWeapons[i].Occ) {
                                    WeaponPair wp3 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    pInf->OccupyWeapon.WeaponType = wp3.Normal; pInf->EliteOccupyWeapon.WeaponType = wp3.Elite;
                                    LogDebug("【全局混沌】步兵 [%s] 驻军 -> 普通[%s] | 精英[%s]", pInf->ID, wp3.Normal->ID, wp3.Elite->ID);
                                }
                            }
                        }
                    }
                    if (randUnits) {
                        for (int i = 0; i < UnitTypeClass::Array.Count; i++) {
                            UnitTypeClass* pUnit = UnitTypeClass::Array.Items[i];
                            if (pUnit && !IsWhitelisted(pUnit->ID) && i < (int)BackupUnitWeapons.size()) {
                                if (BackupUnitWeapons[i].Pri) {
                                    WeaponPair wp1 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    pUnit->Weapon[0].WeaponType = wp1.Normal; pUnit->EliteWeapon[0].WeaponType = wp1.Elite;
                                    LogDebug("【全局混沌】载具 [%s] 主武 -> 普通[%s] | 精英[%s]", pUnit->ID, wp1.Normal->ID, wp1.Elite->ID);
                                }
                                if (BackupUnitWeapons[i].Sec) {
                                    WeaponPair wp2 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    pUnit->Weapon[1].WeaponType = wp2.Normal; pUnit->EliteWeapon[1].WeaponType = wp2.Elite;
                                    LogDebug("【全局混沌】载具 [%s] 副武 -> 普通[%s] | 精英[%s]", pUnit->ID, wp2.Normal->ID, wp2.Elite->ID);
                                }
                            }
                        }
                    }
                    if (randAircraft) {
                        for (int i = 0; i < AircraftTypeClass::Array.Count; i++) {
                            AircraftTypeClass* pAir = AircraftTypeClass::Array.Items[i];
                            if (pAir && !IsWhitelisted(pAir->ID) && i < (int)BackupAircraftWeapons.size()) {
                                if (BackupAircraftWeapons[i].Pri) {
                                    WeaponPair wp1 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    pAir->Weapon[0].WeaponType = wp1.Normal; pAir->EliteWeapon[0].WeaponType = wp1.Elite;
                                    LogDebug("【全局混沌】空军 [%s] 主武 -> 普通[%s] | 精英[%s]", pAir->ID, wp1.Normal->ID, wp1.Elite->ID);
                                }
                                if (BackupAircraftWeapons[i].Sec) {
                                    WeaponPair wp2 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    pAir->Weapon[1].WeaponType = wp2.Normal; pAir->EliteWeapon[1].WeaponType = wp2.Elite;
                                    LogDebug("【全局混沌】空军 [%s] 副武 -> 普通[%s] | 精英[%s]", pAir->ID, wp2.Normal->ID, wp2.Elite->ID);
                                }
                            }
                        }
                    }
                    if (randBuildings) {
                        for (int i = 0; i < BuildingTypeClass::Array.Count; i++) {
                            BuildingTypeClass* pBld = BuildingTypeClass::Array.Items[i];
                            if (pBld && !IsWhitelisted(pBld->ID) && i < (int)BackupBuildingWeapons.size()) {
                                if (BackupBuildingWeapons[i].Pri) {
                                    WeaponPair wp1;
                                    if (includeBldInChaos) wp1 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    else wp1 = SafeBuildingWeapons[rand() % SafeBuildingWeapons.size()];
                                    pBld->Weapon[0].WeaponType = wp1.Normal; pBld->EliteWeapon[0].WeaponType = wp1.Elite;
                                    LogDebug("【全局混沌】建筑 [%s] 主武 -> 普通[%s] | 精英[%s]", pBld->ID, wp1.Normal->ID, wp1.Elite->ID);
                                }
                                if (BackupBuildingWeapons[i].Sec) {
                                    WeaponPair wp2;
                                    if (includeBldInChaos) wp2 = GlobalChaosPool[rand() % GlobalChaosPool.size()];
                                    else wp2 = SafeBuildingWeapons[rand() % SafeBuildingWeapons.size()];
                                    pBld->Weapon[1].WeaponType = wp2.Normal; pBld->EliteWeapon[1].WeaponType = wp2.Elite;
                                    LogDebug("【全局混沌】建筑 [%s] 副武 -> 普通[%s] | 精英[%s]", pBld->ID, wp2.Normal->ID, wp2.Elite->ID);
                                }
                            }
                        }
                    }
                }
                LogDebug("<<< 全局大乱炖完成 <<<");
                Sleep(300);
            }

            // ================== R键：一键还原底层图纸 ==================
            if (pressR)
            {
                LogDebug(">>> 恢复原厂图纸 (%c键) <<<", vkRestore);
                PlayBeep(1000, 200);
                for (int i = 0; i < InfantryTypeClass::Array.Count; i++) {
                    if (i < (int)BackupInfWeapons.size() && InfantryTypeClass::Array.Items[i]) {
                        InfantryTypeClass::Array.Items[i]->Weapon[0].WeaponType = BackupInfWeapons[i].Pri;
                        InfantryTypeClass::Array.Items[i]->Weapon[1].WeaponType = BackupInfWeapons[i].Sec;
                        InfantryTypeClass::Array.Items[i]->EliteWeapon[0].WeaponType = BackupInfWeapons[i].EPri;
                        InfantryTypeClass::Array.Items[i]->EliteWeapon[1].WeaponType = BackupInfWeapons[i].ESec;
                        InfantryTypeClass::Array.Items[i]->OccupyWeapon.WeaponType = BackupInfWeapons[i].Occ;
                        InfantryTypeClass::Array.Items[i]->EliteOccupyWeapon.WeaponType = BackupInfWeapons[i].EOcc;
                    }
                }
                for (int i = 0; i < UnitTypeClass::Array.Count; i++) {
                    if (i < (int)BackupUnitWeapons.size() && UnitTypeClass::Array.Items[i]) {
                        UnitTypeClass::Array.Items[i]->Weapon[0].WeaponType = BackupUnitWeapons[i].Pri;
                        UnitTypeClass::Array.Items[i]->Weapon[1].WeaponType = BackupUnitWeapons[i].Sec;
                        UnitTypeClass::Array.Items[i]->EliteWeapon[0].WeaponType = BackupUnitWeapons[i].EPri;
                        UnitTypeClass::Array.Items[i]->EliteWeapon[1].WeaponType = BackupUnitWeapons[i].ESec;
                    }
                }
                for (int i = 0; i < AircraftTypeClass::Array.Count; i++) {
                    if (i < (int)BackupAircraftWeapons.size() && AircraftTypeClass::Array.Items[i]) {
                        AircraftTypeClass::Array.Items[i]->Weapon[0].WeaponType = BackupAircraftWeapons[i].Pri;
                        AircraftTypeClass::Array.Items[i]->Weapon[1].WeaponType = BackupAircraftWeapons[i].Sec;
                        AircraftTypeClass::Array.Items[i]->EliteWeapon[0].WeaponType = BackupAircraftWeapons[i].EPri;
                        AircraftTypeClass::Array.Items[i]->EliteWeapon[1].WeaponType = BackupAircraftWeapons[i].ESec;
                    }
                }
                for (int i = 0; i < BuildingTypeClass::Array.Count; i++) {
                    if (i < (int)BackupBuildingWeapons.size() && BuildingTypeClass::Array.Items[i]) {
                        BuildingTypeClass::Array.Items[i]->Weapon[0].WeaponType = BackupBuildingWeapons[i].Pri;
                        BuildingTypeClass::Array.Items[i]->Weapon[1].WeaponType = BackupBuildingWeapons[i].Sec;
                        BuildingTypeClass::Array.Items[i]->EliteWeapon[0].WeaponType = BackupBuildingWeapons[i].EPri;
                        BuildingTypeClass::Array.Items[i]->EliteWeapon[1].WeaponType = BackupBuildingWeapons[i].ESec;
                    }
                }
                LogDebug("<<< 恢复完成 <<<");
                Sleep(500); // 恢复图纸需要较高延迟防抖
            }
        }
        Sleep(30); // 线程挂起，防止占用过高导致游戏掉帧
    }
    return 0;
}

// DLL 注入主入口
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        CreateThread(NULL, 0, GodHandThread, NULL, 0, NULL);
    }
    return TRUE;
}