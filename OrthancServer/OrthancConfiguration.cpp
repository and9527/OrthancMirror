/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2018 Osimis S.A., Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders of this
 * program give permission to link the code of its release with the
 * OpenSSL project's "OpenSSL" library (or with modified versions of it
 * that use the same license as the "OpenSSL" library), and distribute
 * the linked executables. You must obey the GNU General Public License
 * in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s) with this exception, you may extend this exception to
 * your version of the file(s), but you are not obligated to do so. If
 * you do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source files
 * in the program, then also delete it here.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "PrecompiledHeadersServer.h"
#include "OrthancConfiguration.h"

#include "../Core/HttpServer/MongooseServer.h"
#include "../Core/Logging.h"
#include "../Core/OrthancException.h"
#include "../Core/SystemToolbox.h"
#include "../Core/Toolbox.h"

#include "ServerIndex.h"


static const char* const DICOM_MODALITIES = "DicomModalities";
static const char* const ORTHANC_PEERS = "OrthancPeers";


namespace Orthanc
{
  static void AddFileToConfiguration(Json::Value& target,
                                     const boost::filesystem::path& path)
  {
    std::map<std::string, std::string> env;
    SystemToolbox::GetEnvironmentVariables(env);
    
    LOG(WARNING) << "Reading the configuration from: " << path;

    Json::Value config;

    {
      std::string content;
      SystemToolbox::ReadFile(content, path.string());

      content = Toolbox::SubstituteVariables(content, env);

      Json::Value tmp;
      Json::Reader reader;
      if (!reader.parse(content, tmp) ||
          tmp.type() != Json::objectValue)
      {
        LOG(ERROR) << "The configuration file does not follow the JSON syntax: " << path;
        throw OrthancException(ErrorCode_BadJson);
      }

      Toolbox::CopyJsonWithoutComments(config, tmp);
    }

    if (target.size() == 0)
    {
      target = config;
    }
    else
    {
      // Merge the newly-added file with the previous content of "target"
      Json::Value::Members members = config.getMemberNames();
      for (Json::Value::ArrayIndex i = 0; i < members.size(); i++)
      {
        if (target.isMember(members[i]))
        {
          LOG(ERROR) << "The configuration section \"" << members[i]
                     << "\" is defined in 2 different configuration files";
          throw OrthancException(ErrorCode_BadFileFormat);          
        }
        else
        {
          target[members[i]] = config[members[i]];
        }
      }
    }
  }

    
  static void ScanFolderForConfiguration(Json::Value& target,
                                         const char* folder)
  {
    using namespace boost::filesystem;

    LOG(WARNING) << "Scanning folder \"" << folder << "\" for configuration files";

    directory_iterator end_it; // default construction yields past-the-end
    for (directory_iterator it(folder);
         it != end_it;
         ++it)
    {
      if (!is_directory(it->status()))
      {
        std::string extension = boost::filesystem::extension(it->path());
        Toolbox::ToLowerCase(extension);

        if (extension == ".json")
        {
          AddFileToConfiguration(target, it->path().string());
        }
      }
    }
  }

    
  static void ReadConfiguration(Json::Value& target,
                                const char* configurationFile)
  {
    target = Json::objectValue;

    if (configurationFile)
    {
      if (!boost::filesystem::exists(configurationFile))
      {
        LOG(ERROR) << "Inexistent path to configuration: " << configurationFile;
        throw OrthancException(ErrorCode_InexistentFile);
      }
      
      if (boost::filesystem::is_directory(configurationFile))
      {
        ScanFolderForConfiguration(target, configurationFile);
      }
      else
      {
        AddFileToConfiguration(target, configurationFile);
      }
    }
    else
    {
#if ORTHANC_STANDALONE == 1
      // No default path for the standalone configuration
      LOG(WARNING) << "Using the default Orthanc configuration";
      return;

#else
      // In a non-standalone build, we use the
      // "Resources/Configuration.json" from the Orthanc source code

      boost::filesystem::path p = ORTHANC_PATH;
      p /= "Resources";
      p /= "Configuration.json";

      AddFileToConfiguration(target, p);
#endif
    }
  }


  static void CheckAlphanumeric(const std::string& s)
  {
    for (size_t j = 0; j < s.size(); j++)
    {
      if (!isalnum(s[j]) && 
          s[j] != '-')
      {
        LOG(ERROR) << "Only alphanumeric and dash characters are allowed "
                   << "in the names of modalities/peers, but found: " << s;
        throw OrthancException(ErrorCode_BadFileFormat);
      }
    }
  }


  void OrthancConfiguration::LoadModalitiesFromJson(const Json::Value& source)
  {
    modalities_.clear();

    if (source.type() != Json::objectValue)
    {
      LOG(ERROR) << "Bad format of the \"" << DICOM_MODALITIES << "\" configuration section";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    Json::Value::Members members = source.getMemberNames();

    for (size_t i = 0; i < members.size(); i++)
    {
      const std::string& name = members[i];
      CheckAlphanumeric(name);

      RemoteModalityParameters modality;
      modality.Unserialize(source[name]);
      modalities_[name] = modality;
    }
  }


  void OrthancConfiguration::SaveModalitiesToJson(Json::Value& target)
  {
    target = Json::objectValue;

    for (Modalities::const_iterator it = modalities_.begin(); it != modalities_.end(); ++it)
    {
      Json::Value modality;
      it->second.Serialize(modality, true /* force advanced format */);

      target[it->first] = modality;
    }
  }

    
  void OrthancConfiguration::LoadPeersFromJson(const Json::Value& source)
  {
    peers_.clear();

    if (source.type() != Json::objectValue)
    {
      LOG(ERROR) << "Bad format of the \"" << ORTHANC_PEERS << "\" configuration section";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    Json::Value::Members members = source.getMemberNames();

    for (size_t i = 0; i < members.size(); i++)
    {
      const std::string& name = members[i];
      CheckAlphanumeric(name);

      WebServiceParameters peer;
      peer.Unserialize(source[name]);
      peers_[name] = peer;
    }
  }


  void OrthancConfiguration::SavePeersToJson(Json::Value& target)
  {
    target = Json::objectValue;

    for (Peers::const_iterator it = peers_.begin(); it != peers_.end(); ++it)
    {
      Json::Value peer;
      it->second.Serialize(peer, 
                           false /* use simple format if possible */, 
                           true  /* include passwords */);

      target[it->first] = peer;
    }
  }  
    
    
  void OrthancConfiguration::LoadModalitiesAndPeers()
  {
    if (json_.isMember(DICOM_MODALITIES))
    {
      LoadModalitiesFromJson(json_[DICOM_MODALITIES]);
    }
    else
    {
      // TODO - Read from DB
      modalities_.clear();
    }

    if (json_.isMember(ORTHANC_PEERS))
    {
      LoadPeersFromJson(json_[ORTHANC_PEERS]);
    }
    else
    {
      // TODO - Read from DB
      peers_.clear();
    }
  }


  void OrthancConfiguration::SaveModalities()
  {
    if (!modalities_.empty() ||
        json_.isMember(DICOM_MODALITIES))
    {
      SaveModalitiesToJson(json_[DICOM_MODALITIES]);
    }

    // TODO - Write to DB
  }


  void OrthancConfiguration::SavePeers()
  {
    if (!peers_.empty() ||
        json_.isMember(ORTHANC_PEERS))
    {
      SavePeersToJson(json_[ORTHANC_PEERS]);
    }

    // TODO - Write to DB
  }


  OrthancConfiguration& OrthancConfiguration::GetInstance()
  {
    static OrthancConfiguration configuration;
    return configuration;
  }


  std::string OrthancConfiguration::GetStringParameter(const std::string& parameter,
                                                       const std::string& defaultValue) const
  {
    if (json_.isMember(parameter))
    {
      if (json_[parameter].type() != Json::stringValue)
      {
        LOG(ERROR) << "The configuration option \"" << parameter << "\" must be a string";
        throw OrthancException(ErrorCode_BadParameterType);
      }
      else
      {
        return json_[parameter].asString();
      }
    }
    else
    {
      return defaultValue;
    }
  }

    
  int OrthancConfiguration::GetIntegerParameter(const std::string& parameter,
                                                int defaultValue) const
  {
    if (json_.isMember(parameter))
    {
      if (json_[parameter].type() != Json::intValue)
      {
        LOG(ERROR) << "The configuration option \"" << parameter << "\" must be an integer";
        throw OrthancException(ErrorCode_BadParameterType);
      }
      else
      {
        return json_[parameter].asInt();
      }
    }
    else
    {
      return defaultValue;
    }
  }

    
  unsigned int OrthancConfiguration::GetUnsignedIntegerParameter(
    const std::string& parameter,
    unsigned int defaultValue) const
  {
    int v = GetIntegerParameter(parameter, defaultValue);

    if (v < 0)
    {
      LOG(ERROR) << "The configuration option \"" << parameter << "\" must be a positive integer";
      throw OrthancException(ErrorCode_ParameterOutOfRange);
    }
    else
    {
      return static_cast<unsigned int>(v);
    }
  }


  bool OrthancConfiguration::GetBooleanParameter(const std::string& parameter,
                                                 bool defaultValue) const
  {
    if (json_.isMember(parameter))
    {
      if (json_[parameter].type() != Json::booleanValue)
      {
        LOG(ERROR) << "The configuration option \"" << parameter
                   << "\" must be a Boolean (true or false)";
        throw OrthancException(ErrorCode_BadParameterType);
      }
      else
      {
        return json_[parameter].asBool();
      }
    }
    else
    {
      return defaultValue;
    }
  }


  void OrthancConfiguration::Read(const char* configurationFile)
  {
    // Read the content of the configuration
    configurationFileArg_ = configurationFile;
    ReadConfiguration(json_, configurationFile);

    // Adapt the paths to the configurations
    defaultDirectory_ = boost::filesystem::current_path();
    configurationAbsolutePath_ = "";

    if (configurationFile)
    {
      if (boost::filesystem::is_directory(configurationFile))
      {
        defaultDirectory_ = boost::filesystem::path(configurationFile);
        configurationAbsolutePath_ = boost::filesystem::absolute(configurationFile).parent_path().string();
      }
      else
      {
        defaultDirectory_ = boost::filesystem::path(configurationFile).parent_path();
        configurationAbsolutePath_ = boost::filesystem::absolute(configurationFile).string();
      }
    }
    else
    {
#if ORTHANC_STANDALONE != 1
      // In a non-standalone build, we use the
      // "Resources/Configuration.json" from the Orthanc source code

      boost::filesystem::path p = ORTHANC_PATH;
      p /= "Resources";
      p /= "Configuration.json";
      configurationAbsolutePath_ = boost::filesystem::absolute(p).string();
#endif
    }

    LoadModalitiesAndPeers();
  }


  void OrthancConfiguration::GetDicomModalityUsingSymbolicName(
    RemoteModalityParameters& modality,
    const std::string& name) const
  {
    Modalities::const_iterator found = modalities_.find(name);

    if (found == modalities_.end())
    {
      LOG(ERROR) << "No modality with symbolic name: " << name;
      throw OrthancException(ErrorCode_InexistentItem);
    }
    else
    {
      modality = found->second;
    }
  }


  bool OrthancConfiguration::LookupOrthancPeer(WebServiceParameters& peer,
                                               const std::string& name) const
  {
    Peers::const_iterator found = peers_.find(name);

    if (found == peers_.end())
    {
      LOG(ERROR) << "No peer with symbolic name: " << name;
      return false;
    }
    else
    {
      peer = found->second;
      return true;
    }
  }


  void OrthancConfiguration::GetListOfDicomModalities(std::set<std::string>& target) const
  {
    target.clear();

    for (Modalities::const_iterator 
           it = modalities_.begin(); it != modalities_.end(); ++it)
    {
      target.insert(it->first);
    }
  }


  void OrthancConfiguration::GetListOfOrthancPeers(std::set<std::string>& target) const
  {
    target.clear();

    for (Peers::const_iterator it = peers_.begin(); it != peers_.end(); ++it)
    {
      target.insert(it->first);
    }
  }


  void OrthancConfiguration::SetupRegisteredUsers(MongooseServer& httpServer) const
  {
    httpServer.ClearUsers();

    if (!json_.isMember("RegisteredUsers"))
    {
      return;
    }

    const Json::Value& users = json_["RegisteredUsers"];
    if (users.type() != Json::objectValue)
    {
      LOG(ERROR) << "Badly formatted list of users";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    Json::Value::Members usernames = users.getMemberNames();
    for (size_t i = 0; i < usernames.size(); i++)
    {
      const std::string& username = usernames[i];
      std::string password = users[username].asString();
      httpServer.RegisterUser(username.c_str(), password.c_str());
    }
  }
    

  std::string OrthancConfiguration::InterpretStringParameterAsPath(
    const std::string& parameter) const
  {
    return SystemToolbox::InterpretRelativePath(defaultDirectory_.string(), parameter);
  }

    
  void OrthancConfiguration::GetListOfStringsParameter(std::list<std::string>& target,
                                                       const std::string& key) const
  {
    target.clear();
  
    if (!json_.isMember(key))
    {
      return;
    }

    const Json::Value& lst = json_[key];

    if (lst.type() != Json::arrayValue)
    {
      LOG(ERROR) << "Badly formatted list of strings";
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    for (Json::Value::ArrayIndex i = 0; i < lst.size(); i++)
    {
      target.push_back(lst[i].asString());
    }    
  }

    
  bool OrthancConfiguration::IsSameAETitle(const std::string& aet1,
                                           const std::string& aet2) const
  {
    if (GetBooleanParameter("StrictAetComparison", false))
    {
      // Case-sensitive matching
      return aet1 == aet2;
    }
    else
    {
      // Case-insensitive matching (default)
      std::string tmp1, tmp2;
      Toolbox::ToLowerCase(tmp1, aet1);
      Toolbox::ToLowerCase(tmp2, aet2);
      return tmp1 == tmp2;
    }
  }


  bool OrthancConfiguration::LookupDicomModalityUsingAETitle(RemoteModalityParameters& modality,
                                                             const std::string& aet) const
  {
    for (Modalities::const_iterator it = modalities_.begin(); it != modalities_.end(); ++it)
    {
      if (IsSameAETitle(aet, it->second.GetApplicationEntityTitle()))
      {
        modality = it->second;
        return true;
      }
    }

    return false;
  }


  bool OrthancConfiguration::IsKnownAETitle(const std::string& aet,
                                            const std::string& ip) const
  {
    RemoteModalityParameters modality;
    
    if (!LookupDicomModalityUsingAETitle(modality, aet))
    {
      LOG(WARNING) << "Modality \"" << aet
                   << "\" is not listed in the \"DicomModalities\" configuration option";
      return false;
    }
    else if (!GetBooleanParameter("DicomCheckModalityHost", false) ||
             ip == modality.GetHost())
    {
      return true;
    }
    else
    {
      LOG(WARNING) << "Forbidding access from AET \"" << aet
                   << "\" given its hostname (" << ip << ") does not match "
                   << "the \"DicomModalities\" configuration option ("
                   << modality.GetHost() << " was expected)";
      return false;
    }
  }


  RemoteModalityParameters 
  OrthancConfiguration::GetModalityUsingSymbolicName(const std::string& name) const
  {
    RemoteModalityParameters modality;
    GetDicomModalityUsingSymbolicName(modality, name);

    return modality;
  }

    
  RemoteModalityParameters 
  OrthancConfiguration::GetModalityUsingAet(const std::string& aet) const
  {
    RemoteModalityParameters modality;
      
    if (LookupDicomModalityUsingAETitle(modality, aet))
    {
      return modality;
    }
    else
    {
      LOG(ERROR) << "Unknown modality for AET: " << aet;
      throw OrthancException(ErrorCode_InexistentItem);
    }
  }

    
  void OrthancConfiguration::UpdateModality(const std::string& symbolicName,
                                            const RemoteModalityParameters& modality)
  {
    modalities_[symbolicName] = modality;
    SaveModalities();
  }


  void OrthancConfiguration::RemoveModality(const std::string& symbolicName)
  {
    modalities_.erase(symbolicName);
    SaveModalities();
  }

    
  void OrthancConfiguration::UpdatePeer(const std::string& symbolicName,
                                        const WebServiceParameters& peer)
  {
    peer.CheckClientCertificate();

    peers_[symbolicName] = peer;
    SavePeers();
  }


  void OrthancConfiguration::RemovePeer(const std::string& symbolicName)
  {
    peers_.erase(symbolicName);
    SavePeers();
  }


  void OrthancConfiguration::Format(std::string& result) const
  {
    Json::StyledWriter w;
    result = w.write(json_);
  }


  void OrthancConfiguration::SetDefaultEncoding(Encoding encoding)
  {
    SetDefaultDicomEncoding(encoding);

    // Propagate the encoding to the configuration file that is
    // stored in memory
    json_["DefaultEncoding"] = EnumerationToString(encoding);
  }


  bool OrthancConfiguration::HasConfigurationChanged() const
  {
    Json::Value current;
    ReadConfiguration(current, configurationFileArg_);

    Json::FastWriter writer;
    std::string a = writer.write(json_);
    std::string b = writer.write(current);

    return a != b;
  }


  void OrthancConfiguration::SetServerIndex(ServerIndex& index)
  {
    serverIndex_ = &index;
  }


  void OrthancConfiguration::ResetServerIndex()
  {
    serverIndex_ = NULL;
  }
}