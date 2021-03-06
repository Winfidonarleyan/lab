/*
 * This file is part of the WarheadCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Config.h"
#include "Log.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include <mutex>
#include <fstream>
#include <unordered_map>

namespace
{
    std::string _filename;
    std::vector<std::string> _additonalFiles;
    std::vector<std::string> _args;
    std::unordered_map<std::string /*name*/, std::string /*value*/> _configOptions;
    std::mutex _configLock;

    // Check system configs like *server.conf*
    bool IsAppConfig(std::string_view fileName)
    {
        size_t found = fileName.find_first_of("authserver.conf");
        if (found != std::string::npos)
            return true;

        found = fileName.find_first_of("worldserver.conf");
        if (found != std::string::npos)
            return true;

        return false;
    }

    template<typename Format, typename... Args>
    inline void PrintError(std::string_view filename, Format&& fmt, Args&& ... args)
    {
        std::string message = Warhead::StringFormat(std::forward<Format>(fmt), std::forward<Args>(args)...);

        LOG_ERROR("%s", message.c_str());
    }

    void AddKey(std::string const& optionName, std::string const& optionKey, bool replace = true)
    {
        auto const& itr = _configOptions.find(optionName);
        if (itr != _configOptions.end())
        {
            if (!replace)
            {
                LOG_ERROR("server", "> Config: Option '%s' is exist! Option key - '%s'", optionName.c_str(), itr->second.c_str());
                return;
            }

            _configOptions.erase(optionName);
        }

        _configOptions.emplace(optionName, optionKey);
    }

    void ParseFile(std::string const& file)
    {
        std::ifstream in(file);

        if (in.fail())
            throw ConfigException(Warhead::StringFormat("Config::LoadFile: Failed open file '%s'", file.c_str()));

        uint32 count = 0;
        uint32 lineNumber = 0;
        std::unordered_map<std::string /*name*/, std::string /*value*/> fileConfigs;

        auto IsDuplicateOption = [&](std::string const& confOption)
        {
            auto const& itr = fileConfigs.find(confOption);
            if (itr != fileConfigs.end())
            {
                PrintError(file, "> Config::LoadFile: Dublicate key name '%s' in config file '%s'", std::string(confOption).c_str(), file.c_str());
                return true;
            }

            return false;
        };

        while (in.good())
        {
            lineNumber++;
            std::string line;
            std::getline(in, line);

            // read line error
            if (!in.good() && !in.eof())
                throw ConfigException(Warhead::StringFormat("> Config::LoadFile: Failure to read line number %u in file '%s'", lineNumber, file.c_str()));

            // remove whitespace in line
            line = Warhead::String::Trim(line, in.getloc());

            if (line.empty())
            {
                continue;
            }

            // comments
            if (line[0] == '#' || line[0] == '[')
            {
                continue;
            }

            size_t found = line.find_first_of('#');
            if (found != std::string::npos)
            {
                line = line.substr(0, found);
            }

            auto const equal_pos = line.find('=');

            if (equal_pos == std::string::npos || equal_pos == line.length())
            {
                PrintError(file, "> Config::LoadFile: Failure to read line number %u in file '%s'. Skip this line", lineNumber, file.c_str());
                continue;
            }

            auto entry = Warhead::String::Trim(line.substr(0, equal_pos), in.getloc());
            auto value = Warhead::String::Trim(line.substr(equal_pos + 1, std::string::npos), in.getloc());

            value.erase(std::remove(value.begin(), value.end(), '"'), value.end());

            // Skip if 2+ same options in one config file
            if (IsDuplicateOption(entry))
                continue;

            // Add to temp container
            fileConfigs.emplace(entry, value);
            count++;
        }

        // No lines read
        if (!count)
            throw ConfigException(Warhead::StringFormat("Config::LoadFile: Empty file '%s'", file.c_str()));

        // Add correct keys if file load without errors
        for (auto const& [entry, key] : fileConfigs)
            AddKey(entry, key);
    }

    bool LoadFile(std::string const& file)
    {
        try
        {
            ParseFile(file);
            return true;
        }
        catch (const std::exception& e)
        {
            PrintError(file, "> %s", e.what());
        }

        return false;
    }
}

bool ConfigMgr::LoadInitial(std::string const& file)
{
    std::lock_guard<std::mutex> lock(_configLock);
    _configOptions.clear();
    return LoadFile(file);
}

bool ConfigMgr::LoadAdditionalFile(std::string file)
{
    std::lock_guard<std::mutex> lock(_configLock);
    return LoadFile(file);
}

ConfigMgr* ConfigMgr::instance()
{
    static ConfigMgr instance;
    return &instance;
}

template<class T>
T ConfigMgr::GetValueDefault(std::string const& name, T const& def, bool showLogs /*= true*/) const
{
    auto const& itr = _configOptions.find(name);
    if (itr == _configOptions.end())
    {
        if (showLogs)
        {
            LOG_ERROR("server", "> Config: Missing name %s in config, add \"%s = %s\"",
                name.c_str(), name.c_str(), Warhead::ToString(def).c_str());
        }

        return def;
    }

    auto value = Warhead::StringTo<T>(itr->second);
    if (!value)
    {
        if (showLogs)
        {
            LOG_ERROR("server", "> Config: Bad value defined for name '%s', going to use '%s' instead",
                name.c_str(), Warhead::ToString(def).c_str());
        }

        return def;
    }

    return *value;
}

template<>
std::string ConfigMgr::GetValueDefault<std::string>(std::string const& name, std::string const& def, bool showLogs /*= true*/) const
{
    auto const& itr = _configOptions.find(name);
    if (itr == _configOptions.end())
    {
        if (showLogs)
        {
            LOG_ERROR("server", "> Config: Missing name %s in config, add \"%s = %s\"",
                name.c_str(), name.c_str(), def.c_str());
        }

        return def;
    }

    return itr->second;
}

template<class T>
T ConfigMgr::GetOption(std::string const& name, T const& def, bool showLogs /*= true*/) const
{
    return GetValueDefault<T>(name, def, showLogs);
}

template<>
WH_COMMON_API bool ConfigMgr::GetOption<bool>(std::string const& name, bool const& def, bool showLogs /*= true*/) const
{
    std::string val = GetValueDefault(name, std::string(def ? "1" : "0"), showLogs);

    auto boolVal = Warhead::StringTo<bool>(val);
    if (!boolVal)
    {
        if (showLogs)
        {
            LOG_ERROR("server", "> Config: Bad value defined for name '%s', going to use '%s' instead",
                name.c_str(), def ? "true" : "false");
        }

        return def;
    }

    return *boolVal;
}

std::vector<std::string> ConfigMgr::GetKeysByString(std::string const& name)
{
    std::lock_guard<std::mutex> lock(_configLock);

    std::vector<std::string> keys;

    for (auto const& [optionName, key] : _configOptions)
        if (!optionName.compare(0, name.length(), name))
            keys.emplace_back(optionName);

    return keys;
}

std::string const& ConfigMgr::GetFilename()
{
    std::lock_guard<std::mutex> lock(_configLock);
    return _filename;
}

std::string const ConfigMgr::GetConfigPath()
{
    std::lock_guard<std::mutex> lock(_configLock);

    return "configs/";
}

void ConfigMgr::Configure(std::string const& initFileName)
{
    _filename = initFileName;
}

bool ConfigMgr::LoadAppConfigs()
{
    // #1 - Load init config file .conf.dist
    if (!LoadInitial(_filename + ".dist"))
        return false;

    // #2 - Load .conf file
    /*if (!LoadAdditionalFile(_filename))
        return false;*/

    return true;
}

/*
 * End deprecated geters
 */

#define TEMPLATE_CONFIG_OPTION(__typename) \
    template WH_COMMON_API __typename ConfigMgr::GetOption<__typename>(std::string const& name, __typename const& def, bool showLogs /*= true*/) const;

TEMPLATE_CONFIG_OPTION(std::string)
TEMPLATE_CONFIG_OPTION(uint8)
TEMPLATE_CONFIG_OPTION(int8)
TEMPLATE_CONFIG_OPTION(uint16)
TEMPLATE_CONFIG_OPTION(int16)
TEMPLATE_CONFIG_OPTION(uint32)
TEMPLATE_CONFIG_OPTION(int32)
TEMPLATE_CONFIG_OPTION(uint64)
TEMPLATE_CONFIG_OPTION(int64)
TEMPLATE_CONFIG_OPTION(float)

#undef TEMPLATE_CONFIG_OPTION
