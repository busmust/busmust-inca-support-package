#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <bmapi.h>
namespace bmj {
std::wstring moduleDirectory();
std::string identity(const BM_ChannelInfoTypeDef&);
enum class SelectionMode { Auto, Manual };
struct Selection {
    SelectionMode mode=SelectionMode::Auto;
    std::string device;
    bool termination=false, customSampling=false, classicSampling=false;
    unsigned nominalSample=75, dataSample=80;
};
enum class SelectionStatus { Missing, Present, Invalid, InvalidSnapshot };
SelectionStatus readSelection(Selection&);
bool savedTermination(const std::string&,bool&,const Selection* snapshot=nullptr);
bool saveSelection(const Selection&);
struct Api {
    HMODULE module = nullptr;
    bool initialized = false;
    decltype(&BM_Init) init = nullptr;
    decltype(&BM_UnInit) uninit = nullptr;
    decltype(&BM_Enumerate) enumerate = nullptr;
    decltype(&BM_OpenEx) open = nullptr;
    decltype(&BM_Close) close = nullptr;
    decltype(&BM_Read) read = nullptr;
    decltype(&BM_Write) write = nullptr;
    decltype(&BM_SetBitrate) bitrate = nullptr;
    decltype(&BM_GetBitrate) getBitrate = nullptr;
    decltype(&BM_SetCanMode) mode = nullptr;
    decltype(&BM_GetCanMode) getMode = nullptr;
    decltype(&BM_SetTerminalRegister) termination = nullptr;
    decltype(&BM_GetTerminalRegister) getTermination = nullptr;
    decltype(&BM_ClearBufferEx) clear = nullptr;
    decltype(&BM_ClearBuffer) clearAll = nullptr;
    decltype(&BM_RecoverBusOff) recoverBusOff = nullptr;
    decltype(&BM_GetStatus) status = nullptr;
    decltype(&BM_GetTimestamp) timestamp = nullptr;
    decltype(&BM_GetVersion) version = nullptr;
    bool load(std::string& error);
    void unload();
    BM_StatusTypeDef list(std::vector<BM_ChannelInfoTypeDef>&);
    BM_StatusTypeDef clearDirection(BM_ChannelHandle handle,BM_BufferId direction) const {
        const BM_StatusTypeDef result=clear(handle,direction);
        return result==BM_ERROR_ILLOPERATION ? BM_ERROR_OK : result;
    }
};
}
