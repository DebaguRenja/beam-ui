// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <QObject>
#include <QtWebEngineWidgets/QWebEngineView>
#include <QWebEngineProfile>
#include <QFileDialog>
#include <qbuffer.h>
#include "apps_view.h"
#include "utility/logger.h"
#include "model/app_model.h"
#include "version.h"
#include "quazip/quazip.h"
#include "quazip/quazipfile.h"
#include "quazip/JlCompress.h"
#include "viewmodel/qml_globals.h"
#include "wallet/client/apps_api/apps_utils.h"

namespace
{
    const uint8_t kCountDAppVersionParts = 4;

    QString fromHex(const std::string& value)
    {
        auto tmp = beam::from_hex(value);

        return QString::fromUtf8(reinterpret_cast<char*>(tmp.data()), static_cast<int>(tmp.size()));
    }

    std::string extractStringField(nlohmann::json& json, const char* fieldName)
    {
        const auto& field = json[fieldName];
        if (!field.is_string())
        {
            std::stringstream ss;
            ss << "Invalid " << fieldName << " of the dapp";
            throw std::runtime_error(ss.str());
        }
        return field.get<std::string>();
    }

    QString parseStringField(nlohmann::json& json, const char* fieldName)
    {
        return QString::fromStdString(extractStringField(json, fieldName));
    }

    QString decodeStringField(nlohmann::json& json, const char* fieldName)
    {
        return fromHex(extractStringField(json, fieldName));
    }

    std::string toHex(const QString& value)
    {
        std::string tmp = value.toStdString();
        return beam::to_hex(tmp.data(), tmp.size());
    }

    QVariantMap parsePublisherInfo(nlohmann::json& info)
    {
        QVariantMap tmp;

        tmp["publisherKey"] = QString::fromStdString(info["pubkey"].get<std::string>());
        tmp["nickname"] = fromHex(info["name"].get<std::string>());
        tmp["shortTitle"] = fromHex(info["short_title"].get<std::string>());
        tmp["aboutMe"] = fromHex(info["about_me"].get<std::string>());
        tmp["website"] = fromHex(info["website"].get<std::string>());
        tmp["twitter"] = fromHex(info["twitter"].get<std::string>());
        tmp["linkedin"] = fromHex(info["linkedin"].get<std::string>());
        tmp["instagram"] = fromHex(info["instagram"].get<std::string>());
        tmp["telegram"] = fromHex(info["telegram"].get<std::string>());
        tmp["discord"] = fromHex(info["discord"].get<std::string>());

        return tmp;
    }

    QString removeFilePrefix(QString fname)
    {
        // Some shells/systems provide incorrect count of '/' after file:
        // For example in gnome on linux one '/' is missing. So this ugly code is necessary
        if (fname.startsWith("file:"))
        {
            fname = fname.remove(0, 5);
            while (fname.startsWith("/"))
            {
                fname = fname.remove(0, 1);
            }

#ifndef WIN32
            fname = QString("/") + fname;
#endif
        }
        return fname;
    }

    int16_t compareDAppVersion(const QString& first, const QString& second)
    {
        auto fillDAppVersion = [] (QStringList& versionList) {
            while (versionList.length() < kCountDAppVersionParts)
            {
                versionList.append("0");
            }
        };

        auto firstStringList = first.split(".");
        auto secondStringList = second.split(".");
        fillDAppVersion(firstStringList);
        fillDAppVersion(secondStringList);

        for (uint8_t i = 0; i < firstStringList.length(); ++i)
        {
            if (firstStringList[i] == secondStringList[i])
            {
                continue;
            }
            
            return firstStringList[i].toInt() > secondStringList[i].toInt() ? 1 : -1;
        }
        return 0;
    }
}

namespace beamui::applications
{
    AppsViewModel::AppsViewModel()
        : m_walletModel(AppModel::getInstance().getWalletModel())
    {
        LOG_INFO() << "AppsViewModel created";

        connect(m_walletModel.get(), &WalletModel::transactionsChanged, this, &AppsViewModel::onTransactionsChanged);

        auto defaultProfile = QWebEngineProfile::defaultProfile();
        defaultProfile->setHttpCacheType(QWebEngineProfile::HttpCacheType::DiskHttpCache);
        defaultProfile->setPersistentCookiesPolicy(QWebEngineProfile::PersistentCookiesPolicy::AllowPersistentCookies);
        defaultProfile->setCachePath(AppSettings().getAppsCachePath());
        defaultProfile->setPersistentStoragePath(AppSettings().getAppsStoragePath());

        _userAgent  = defaultProfile->httpUserAgent() + " BEAM/" + QString::fromStdString(PROJECT_VERSION);
        _serverAddr = QString("127.0.0.1:") + QString::number(AppSettings().getAppsServerPort());

        loadApps();
        loadPublishers();
        loadMyPublisherInfo();
    }

    AppsViewModel::~AppsViewModel()
    {
        if (_server)
        {
            _server.reset();
        }
        LOG_INFO() << "AppsViewModel destroyed";
    }

    void AppsViewModel::onCompleted(QObject *webView)
    {
        assert(webView != nullptr);
    }

    QString AppsViewModel::getAppsUrl() const
    {
        auto& settings = AppModel::getInstance().getSettings();
        return settings.getAppsUrl();
    }

    QString AppsViewModel::getAppCachePath(const QString& appid) const
    {
        return AppSettings().getAppsStoragePath(appid);
    }

    QString AppsViewModel::getAppStoragePath(const QString& appid) const
    {
        return AppSettings().getAppsCachePath(appid);
    }

    QString AppsViewModel::getUserAgent() const
    {
        return _userAgent;
    }

    QVariantMap AppsViewModel::parseAppManifest(QTextStream& in, const QString& appFolder)
    {
        QVariantMap app;

        const auto content = in.readAll();
        if (content.isEmpty())
        {
            throw std::runtime_error("Failed to read the manifest file");
        }

        const auto utf = content.toUtf8();

        // do not make json const or it will throw on missing keys
        auto json = nlohmann::json::parse(utf.begin(), utf.end());
        if (!json.is_object() || json.empty())
        {
            throw std::runtime_error("Invalid manifest file");
        }

        const auto& guid = json["guid"];
        if (!guid.is_string() || guid.empty())
        {
            throw std::runtime_error("Invalid GUID in the manifest file");
        }
        app.insert("guid", QString::fromStdString(guid.get<std::string>()));

        const auto& desc = json["description"];
        if (!desc.is_string() || desc.empty())
        {
            throw std::runtime_error("Invalid description in the manifest file");
        }
        app.insert("description", QString::fromStdString(desc.get<std::string>()));

        const auto& name = json["name"];
        if (!name.is_string() || name.empty())
        {
            throw std::runtime_error("Invalid app name in the manifest file");
        }

        const auto sname = name.get<std::string>();
        app.insert("name", QString::fromStdString(sname));

        const auto& url = json["url"];
        if (!url.is_string() || url.empty())
        {
            throw std::runtime_error("Invalid url in the manifest file");
        }

        const auto surl = url.get<std::string>();
        app.insert("url", expandLocalUrl(appFolder, surl));

        const auto& icon = json["icon"];
        if (!icon.empty())
        {
            if (!icon.is_string())
            {
                throw std::runtime_error("Invalid icon in the manifest file");
            }

            const auto ipath = expandLocalFile(appFolder, icon.get<std::string>());
            app.insert("icon", ipath);
            LOG_INFO() << "App: " << sname << ", icon: " << ipath.toStdString();
        }

        const auto& av = json["api_version"];
        if (!av.empty())
        {
            if (!av.is_string())
            {
                throw std::runtime_error("Invalid api_version in the manifest file");
            }
            app.insert("api_version", QString::fromStdString(av.get<std::string>()));
        }

        const auto& mav = json["min_api_version"];
        if (!mav.empty())
        {
            if (!mav.is_string())
            {
                throw std::runtime_error("Invalid min_api_version in the manifest file");
            }
            app.insert("min_api_version", QString::fromStdString(mav.get<std::string>()));
        }

        const auto& v = json["version"];
        if (!v.empty())
        {
            if (!v.is_string())
            {
                throw std::runtime_error("Invalid version in the manifest file");
            }
            app.insert("version", QString::fromStdString(v.get<std::string>()));
        }

        const auto& categoryObj = json["category"];
        if (!categoryObj.empty())
        {
            if (!categoryObj.is_number_unsigned())
            {
                throw std::runtime_error("Invalid category in the manifest file");
            }
            app.insert("category", categoryObj.get<uint32_t>());
        }

        const auto& publisherObj = json["publisher"];
        if (!publisherObj.empty())
        {
            if (!publisherObj.is_string())
            {
                throw std::runtime_error("Invalid publisher in the manifest file");
            }
            app.insert("publisher", QString::fromStdString(publisherObj.get<std::string>()));
        }

        // TODO: Make guid is required field after fixing all DApps
        const auto& guidObj = json["guid"];
        if (!guidObj.empty())
        {
            if (!guidObj.is_string())
            {
                throw std::runtime_error("Invalid 'guid' in the manifest file");
            }
            app.insert("guid", QString::fromStdString(guidObj.get<std::string>()));
        }

        app.insert("local", true);
        // TODO: check why we used surl instead of extended url - app["url"]
        const auto appid = beam::wallet::GenerateAppID(sname, app["url"].toString().toStdString());
        app.insert("appid", QString::fromStdString(appid));

        return app;
    }

    void AppsViewModel::loadApps()
    {
        // TODO: separate dev/local/"from store DApps"
        // load local apps
        _apps = getLocalApps();

        // load server apps
        QPointer<AppsViewModel> guard(this);
        AppModel::getInstance().getWalletModel()->getAsync()->getAppsList(
            [this, guard](bool isOk, const std::string& response)
            {
                if (!guard)
                {
                    return;
                }

                try
                {
                    if (!isOk)
                    {
                        throw std::runtime_error("unsuccessful request");
                    }

                    auto json = nlohmann::json::parse(response);

                    // parse & verify
                    if (json.empty() || !json.is_array())
                    {
                        throw std::runtime_error("invalid response");
                    }

                    for (auto& item : json.items())
                    {
                        QVariantMap app;
                        auto name = parseStringField(item.value(), "name");
                        auto url = parseStringField(item.value(), "url");
                        const auto appid = beam::wallet::GenerateAppID(name.toStdString(), url.toStdString());
                        
                        app.insert("appid", QString::fromStdString(appid));
                        app.insert("description", parseStringField(item.value(), "description"));
                        app.insert("name", name);
                        app.insert("url", url);
                        app.insert("icon", parseStringField(item.value(), "icon"));
                    
                        // TODO: add verification
                        bool isSupported = true;

                        app.insert("supported", isSupported);

                        // TODO: check order of the DApps
                        _apps.push_back(app);
                    }
                }
                catch (const std::runtime_error& err)
                {
                    // TODO: mb need to transfer the error to QML(errorMessage)
                    LOG_WARNING() << "Failed to load remote applications list, " << err.what();
                }
                loadAppsFromStore();
            });
    }

    void AppsViewModel::loadAppsFromStore()
    {
        std::string args = "action=view_dapps,cid=";
        args += AppSettings().getDappStoreCID();

        QPointer<AppsViewModel> guard(this);

        AppModel::getInstance().getWalletModel()->getAsync()->callShaderAndStartTx(AppSettings().getDappStorePath(), args,
            [this, guard](const std::string& err, const std::string& output, const beam::wallet::TxID& id)
            {
                if (!guard)
                {
                    return;
                }

                if (!err.empty())
                {
                    LOG_ERROR() << "Failed to load dapps list from DApp Store" << ", " << err;
                    return;
                }

                try
                {
                    auto json = nlohmann::json::parse(output);

                    if (json.empty() || !json.is_object() || !json["dapps"].is_array())
                    {
                        throw std::runtime_error("Invalid response of the view_dapps method");
                    }

                    for (auto& item : json["dapps"].items())
                    {
                        if (!item.value().is_object())
                        {
                            throw std::runtime_error("Invalid body of the dapp " + item.key());
                        }
                        auto guid = parseStringField(item.value(), "id");
                        auto publisher = parseStringField(item.value(), "publisher");

                        LOG_DEBUG() << "Parsing DApp from contract, guid - " << guid.toStdString() << ", publisher - " << publisher.toStdString();

                        // parse version
                        auto versionObj = item.value()["version"];

                        if (versionObj.empty() || !versionObj.is_object())
                        {
                            throw std::runtime_error("Invalid 'version' of the dapp");
                        }

                        auto majorObj = versionObj["major"];
                        auto minorObj = versionObj["minor"];
                        auto releaseObj = versionObj["release"];
                        auto buildObj = versionObj["build"];
                        if (majorObj.empty() || !majorObj.is_number_unsigned() ||
                            minorObj.empty() || !minorObj.is_number_unsigned() ||
                            releaseObj.empty() || !releaseObj.is_number_unsigned() ||
                            buildObj.empty() || !buildObj.is_number_unsigned())
                        {
                            throw std::runtime_error("Invalid 'version' of the dapp");
                        }

                        QString version;
                        QTextStream textStream(&version);
                        textStream << majorObj.get<uint32_t>() << '.' << minorObj.get<uint32_t>() << '.'
                            << releaseObj.get<uint32_t>() << '.' << buildObj.get<uint32_t>();

                        // try to find in _apps
                        // if found -> installed -> compare version -> set "hasUpdate"
                        // find app in _apps by guid
                        const auto it = std::find_if(_apps.begin(), _apps.end(),
                            [guid](const auto& app) -> bool {
                                const auto appIt = app.find("guid");
                                if (appIt == app.end())
                                {
                                    return false;
                                }
                                return appIt->toString() == guid;
                            }
                        );

                        if (it != _apps.end())
                        {
                            auto& app = *it;
                            if (compareDAppVersion(version, app["version"].toString()) > 0)
                            {
                                app.insert("hasUpdate", true);
                            }
                            app.insert("ipfs_id", parseStringField(item.value(), "ipfs_id"));
                        }
                        else
                        {
                            QMap<QString, QVariant> app;
                            app.insert("description", decodeStringField(item.value(), "description"));
                            app.insert("name", decodeStringField(item.value(), "name"));
                            app.insert("ipfs_id", parseStringField(item.value(), "ipfs_id"));
                            // TODO: check if empty url is allowed for not installed app
                            app.insert("url", "");
                            app.insert("api_version", decodeStringField(item.value(), "api_ver"));
                            app.insert("min_api_version", decodeStringField(item.value(), "min_api_ver"));
                            app.insert("guid", guid);
                            app.insert("publisher", publisher);

                            // TODO: add verification
                            app.insert("supported", true);

                            app.insert("notInstalled", true);

                            _apps.push_back(app);
                        }
                    }
                    emit appsChanged();
                }
                catch (std::runtime_error& err)
                {
                    LOG_ERROR() << "Error while parsing app from contract" << ", " << err.what();
                }
            }
        );
    }

    void AppsViewModel::loadPublishers()
    {
        std::string args = "action=view_publishers,cid=";
        args += AppSettings().getDappStoreCID();

        QPointer<AppsViewModel> guard(this);

        AppModel::getInstance().getWalletModel()->getAsync()->callShaderAndStartTx(AppSettings().getDappStorePath(), args,
            [this, guard](const std::string& err, const std::string& output, const beam::wallet::TxID& id)
            {
                if (!guard)
                {
                    return;
                }

                if (!err.empty())
                {
                    LOG_WARNING() << "Failed to my publisher info" << ", " << err;
                    return;
                }

                try
                {
                    auto json = nlohmann::json::parse(output);

                    LOG_INFO() << json.dump(4);

                    if (json.empty() || !json.is_object() || !json["publishers"].is_array())
                    {
                        throw std::runtime_error("Invalid response of the view_publishers method");
                    }

                    QList<QVariantMap> publishers;

                    for (auto& item : json["publishers"].items())
                    {
                        if (!item.value().is_object())
                        {
                            throw std::runtime_error("Invalid body of the publishers list " + item.key());
                        }

                        publishers.push_back(parsePublisherInfo(item.value()));
                    }

                    setPublishers(publishers);
                }
                catch (std::runtime_error& err)
                {
                    LOG_WARNING() << "Error while parsing publisher from contract" << ", " << err.what();
                }
            }
        );
    }

    void AppsViewModel::loadMyPublisherInfo(bool hideTxIsSentDialog, bool showYouArePublsherDialog)
    {
        std::string args = "action=my_publisher_info,cid=";
        args += AppSettings().getDappStoreCID();

        QPointer<AppsViewModel> guard(this);

        AppModel::getInstance().getWalletModel()->getAsync()->callShaderAndStartTx(AppSettings().getDappStorePath(), args,
            [this, guard, hideTxIsSentDialog, showYouArePublsherDialog](const std::string& err, const std::string& output, const beam::wallet::TxID& id)
            {
                if (!guard)
                {
                    return;
                }

                if (!err.empty())
                {
                    LOG_WARNING() << "Failed to my publisher info" << ", " << err;
                    return;
                }
                else
                {
                    try
                    {
                        auto json = nlohmann::json::parse(output);

                        LOG_INFO() << json.dump(4);

                        if (json.empty() || !json.is_object() || !json["my_publisher_info"].is_object())
                        {
                            throw std::runtime_error("Invalid response of the view_publishers method");
                        }

                        if (!json["my_publisher_info"].empty())
                        {
                            auto& info = json["my_publisher_info"];
                            QVariantMap tmp = parsePublisherInfo(info);

                            setPublisherInfo(tmp);

                            _isPublisher = true;
                            emit isPublisherChanged();
                        }
                    }
                    catch (std::runtime_error& err)
                    {
                        LOG_WARNING() << "Error while parsing publisher from contract" << ", " << err.what();
                    }
                }

                if (hideTxIsSentDialog)
                {
                    emit hideTxIsSent();
                }

                if (showYouArePublsherDialog)
                {
                    emit showYouArePublisher();
                }
            }
        );
    }

    void AppsViewModel::setPublishers(const QList<QVariantMap>& value)
    {
        if (value != _publishers)
        {
            _publishers = value;
            emit publishersChanged();
        }
    }

    QList<QVariantMap> AppsViewModel::getPublishers()
    {
        return _publishers;
    }

    QList<QVariantMap> AppsViewModel::getApps()
    {
        return _apps;
    }

    QList<QVariantMap> AppsViewModel::getLocalApps()
    {
        QList<QVariantMap> result;

        //
        // Dev App
        //
        if (!AppSettings().getDevAppName().isEmpty())
        {
            QVariantMap devapp;

            const auto name  = AppSettings().getDevAppName();
            const auto url   = AppSettings().getDevAppUrl();
            const auto appid = QString::fromStdString(beam::wallet::GenerateAppID(name.toStdString(), url.toStdString()));

            //% "This is your dev application"
            devapp.insert("description",     qtTrId("apps-devapp"));
            devapp.insert("name",            name);
            devapp.insert("url",             url);
            devapp.insert("api_version",     AppSettings().getDevAppApiVer());
            devapp.insert("min_api_version", AppSettings().getDevAppMinApiVer());
            devapp.insert("appid", appid);
            result.push_back(devapp);
        }

        QDir appsdir(AppSettings().getLocalAppsPath());
        auto list = appsdir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);

        for (const auto& finfo: list)
        {
            const auto fullFolder = finfo.absoluteFilePath();
            const auto justFolder = finfo.fileName();
            auto mpath = QDir(fullFolder).absoluteFilePath("manifest.json");

            try
            {
                QFile file(mpath);
                if (!file.open(QFile::ReadOnly | QFile::Text))
                {
                    throw std::runtime_error("Cannot open file");
                }

                QTextStream in(&file);

                auto app = parseAppManifest(in, justFolder);
                app.insert("full_path", fullFolder);

                // TODO: add verification
                app.insert("supported", true);

                result.push_back(app);
            }
            catch(std::runtime_error& err)
            {
                LOG_WARNING() << "Error while reading local app from " << mpath.toStdString() << ", " << err.what();
            }
        }

        _lastLocalApps = result;
        return result;
    }

    bool AppsViewModel::isPublisher() const
    {
        // TODO: check after implementation "becomePublisher"
        return _isPublisher;
    }

    QVariantMap AppsViewModel::getPublisherInfo() const
    {
        return _publisherInfo;
    }

    void AppsViewModel::setPublisherInfo(const QVariantMap& value)
    {
        if (value != _publisherInfo)
        {
            _publisherInfo = value;
            emit publisherInfoChanged();
        }
    }

    QString AppsViewModel::addPublisherByKey(const QString& publisherKey)
    {
        // find publisher in _publishers by publicKey
        const auto it = std::find_if(_publishers.cbegin(), _publishers.cend(),
            [publisherKey] (const auto& publisher) -> bool {
                const auto publisherIt = publisher.find("publisherKey");
                if (publisherIt == publisher.end())
                {
                    assert(false);
                    return false;
                }
                return publisherIt->toString() == publisherKey;
            }
        );

        if (it == _publishers.end())
        {
            assert(false);
            return {};
        }

        // TODO: add publisher to _myPublishers

        return (*it)["nickname"].toString();
    }

    void AppsViewModel::createPublisher(const QVariantMap& publisherInfo)
    {
        std::string args = "action=add_publisher,cid=";
        args += AppSettings().getDappStoreCID();
        args += ",name=" + toHex(publisherInfo["nickname"].toString());
        args += ",short_title=" + toHex(publisherInfo["shortTitle"].toString());
        args += ",about_me=" + toHex(publisherInfo["aboutMe"].toString());
        args += ",website=" + toHex(publisherInfo["website"].toString());
        args += ",twitter=" + toHex(publisherInfo["twitter"].toString());
        args += ",linkedin=" + toHex(publisherInfo["linkedin"].toString());
        args += ",instagram=" + toHex(publisherInfo["instagram"].toString());
        args += ",telegram=" + toHex(publisherInfo["telegram"].toString());
        args += ",discord=" + toHex(publisherInfo["discord"].toString());

        QPointer<AppsViewModel> guard(this);

        AppModel::getInstance().getWalletModel()->getAsync()->callShader(AppSettings().getDappStorePath(), args,
            [this, guard](const std::string& err, const std::string& output, const beam::ByteBuffer& data)
            {
                if (!guard)
                {
                    return;
                }

                if (!err.empty())
                {
                    LOG_WARNING() << "Failed to create a publisher" << ", " << err;
                    return;
                }

                handleShaderTxData(Action::CreatePublisher, data);
            }
        );
    }

    void AppsViewModel::changePublisherInfo(const QVariantMap& publisherInfo)
    {
        std::string args = "action=update_publisher,cid=";
        args += AppSettings().getDappStoreCID();
        args += ",name=" + toHex(publisherInfo["nickname"].toString());
        args += ",short_title=" + toHex(publisherInfo["shortTitle"].toString());
        args += ",about_me=" + toHex(publisherInfo["aboutMe"].toString());
        args += ",website=" + toHex(publisherInfo["website"].toString());
        args += ",twitter=" + toHex(publisherInfo["twitter"].toString());
        args += ",linkedin=" + toHex(publisherInfo["linkedin"].toString());
        args += ",instagram=" + toHex(publisherInfo["instagram"].toString());
        args += ",telegram=" + toHex(publisherInfo["telegram"].toString());
        args += ",discord=" + toHex(publisherInfo["discord"].toString());

        QPointer<AppsViewModel> guard(this);

        AppModel::getInstance().getWalletModel()->getAsync()->callShader(AppSettings().getDappStorePath(), args,
            [this, guard](const std::string& err, const std::string& output, const beam::ByteBuffer& data)
            {
                if (!guard)
                {
                    return;
                }

                if (!err.empty())
                {
                    LOG_WARNING() << "Failed to change a publisher info" << ", " << err;
                    return;
                }

                handleShaderTxData(Action::UpdatePublisher, data);
            }
        );
    }

    bool AppsViewModel::uninstallLocalApp(const QString& appid)
    {
        const auto it = std::find_if(_lastLocalApps.begin(), _lastLocalApps.end(), [appid](const auto& props) -> bool {
            const auto ait = props.find("appid");
            if (ait == props.end())
            {
                assert(false);
                return false;
            }
            return ait->toString() == appid;
        });

        if (it == _lastLocalApps.end())
        {
            assert(false);
            return false;
        }

        const auto pathit = it->find("full_path");
        if (pathit == it->end())
        {
            assert(false);
            return false;
        }

        const auto path = pathit->toString();
        if (path.isEmpty())
        {
            assert(false);
            return false;
        }

        LOG_INFO() << "Deleting local app in folder " << path.toStdString();

        QDir dir(path);
        bool result = dir.removeRecursively();
        if (result)
        {
            // refresh
            loadApps();
        }
        return result;
    }

    QString AppsViewModel::expandLocalUrl(const QString& folder, const std::string& url) const
    {
        QString result = QString::fromStdString(url);
        result.replace("localapp", QString("http://") + _serverAddr + "/" + folder);
        return result;
    }

    QString AppsViewModel::expandLocalFile(const QString& folder, const std::string& url) const
    {
        auto path = QDir(AppSettings().getLocalAppsPath()).filePath(folder);
        auto result = QString::fromStdString(url);
        result.replace("localapp", QString("file:///") + path);
        return result;
    }

    void AppsViewModel::launchAppServer()
    {
        if(!_server)
        {
            try
            {
                _server = std::make_unique<AppsServer>(AppSettings().getLocalAppsPath(),
                                                       AppSettings().getAppsServerPort());
            }
            catch(std::runtime_error& err)
            {
                LOG_WARNING() << "Failed to launch local apps server: " << err.what();
            }
        }
    }

    QString AppsViewModel::chooseFile(const QString& title)
    {
        QFileDialog dialog(nullptr,
                           title,
                           "",
                           "BEAM DApp files (*.dapp)");

        dialog.setWindowModality(Qt::WindowModality::ApplicationModal);
        if (!dialog.exec())
        {
            return "";
        }
        return dialog.selectedFiles().value(0);
    }

    QVariantMap AppsViewModel::getDAppFileProperties(const QString& fname)
    {
        QFileInfo fileInfo(fname);

        QVariantMap properties;
        properties.insert("name", fileInfo.fileName());
        properties.insert("size", fileInfo.size());

        return properties;
    }

    QVariantMap AppsViewModel::parseDAppFile(const QString& fname)
    {
        try
        {
            auto dappFilePath = removeFilePrefix(fname);
            QuaZip zip(dappFilePath);
            if (!zip.open(QuaZip::Mode::mdUnzip))
            {
                throw std::runtime_error("Failed to open the DApp file");
            }

            QVariantMap app;
            for (bool ok = zip.goToFirstFile(); ok; ok = zip.goToNextFile())
            {
                const auto zipFname = zip.getCurrentFileName();
                if (zipFname == "manifest.json")
                {
                    QuaZipFile mfile(zip.getZipName(), zipFname);
                    if (!mfile.open(QIODevice::ReadOnly))
                    {
                        throw std::runtime_error("Failed to read the DApp file");
                    }

                    QTextStream in(&mfile);
                    app = parseAppManifest(in, "");
                }
            }

            if (app["guid"].value<QString>().isEmpty())
            {
                throw std::runtime_error("Invalid DApp file");
            }
            
            // add estimated release_date
            app.insert("release_date", QDate::currentDate().toString("dd MMM yyyy"));

            // preload DApp file
            QFile dappFile(dappFilePath);

            if (!dappFile.open(QFile::ReadOnly))
            {
                throw std::runtime_error("Failed to read the DApp file");
            }

            auto buffer = dappFile.readAll();
            _loadedDAppBuffer = beam::ByteBuffer(buffer.cbegin(), buffer.cend());
            _loadedDApp = app;

            return app;
        }
        catch (std::runtime_error& err)
        {
            LOG_ERROR() << "Failed to parse DApp: " << err.what();
        }
        return {};
    }

    void AppsViewModel::publishDApp(bool isUpdating)
    {
        auto ipfs = AppModel::getInstance().getWalletModel()->getIPFS();
        QPointer<AppsViewModel> guard(this);

        if (!_loadedDApp.has_value() || !_loadedDAppBuffer.has_value())
        {
            assert(false);
            LOG_ERROR() << "Failed to publish DApp, empty buffers.";
            return;
        }

        ipfs->AnyThread_add(std::move(*_loadedDAppBuffer),
            [this, guard, app = std::move(*_loadedDApp), isUpdating](std::string&& ipfsID) mutable
            {
                if (!guard)
                {
                    return;
                }

                LOG_INFO() << "IPFS_ID: " << ipfsID;

                // save result to contract
                uploadAppToStore(std::move(app), ipfsID, isUpdating);
            },
            [](std::string&& err) {
                LOG_ERROR() << "Failed to add to ipfs: " << err;
            }
        );
    }

    bool AppsViewModel::checkDAppNewVersion(const QVariantMap& currentDApp, const QVariantMap& newDApp)
    {
        if (currentDApp["guid"].value<QString>() == currentDApp["guid"].value<QString>())
        {
            return compareDAppVersion(newDApp["version"].value<QString>(), currentDApp["version"].value<QString>()) > 0;
        }
        return false;
    }

    void AppsViewModel::uploadAppToStore(QVariantMap&& app, const std::string& ipfsID, bool isUpdating)
    {
        QString guid = app["guid"].value<QString>();
        QString appName = app["name"].value<QString>();
        QString description = app["description"].value<QString>();

        std::stringstream argsStream;
        argsStream << (isUpdating ? "action=update_dapp," : "action=add_dapp,");
        argsStream << "cid=" << AppSettings().getDappStoreCID().c_str();
        argsStream << ",ipfs_id=" << ipfsID;
        argsStream << ",name=" << toHex(appName);
        argsStream << ",id=" << guid.toStdString();
        argsStream << ",description=" << toHex(description);
        argsStream << ",api_ver=" << toHex(app["api_version"].value<QString>());
        argsStream << ",min_api_ver=" << toHex(app["min_api_version"].value<QString>());
        argsStream << ",category=" << app["category"].value<uint32_t>();

        // parse version
        QStringList version = app["version"].value<QString>().split(".");

        for (; version.length() < kCountDAppVersionParts;)
        {
            version.append("0");
        }

        argsStream << ",major=" << version[0].toStdString();
        argsStream << ",minor=" << version[1].toStdString();
        argsStream << ",release=" << version[2].toStdString();
        argsStream << ",build=" << version[3].toStdString();

        QPointer<AppsViewModel> guard(this);

        LOG_INFO() << "args: " << argsStream.str();

        AppModel::getInstance().getWalletModel()->getAsync()->callShader(AppSettings().getDappStorePath(), argsStream.str(),
            [this, guard](const std::string& err, const std::string& output, const beam::ByteBuffer& data)
            {
                if (!guard)
                {
                    return;
                }

                if (!err.empty())
                {
                    LOG_WARNING() << "Failed to publish app" << ", " << err;
                    return;
                }
                
                if (data.empty())
                {
                    LOG_WARNING() << "Failed to publish app" << ", " << output;
                    return;
                }
                handleShaderTxData(Action::UploadDApp, data);
            }
        );
    }

    void AppsViewModel::installApp(const QString& guid)
    {
        try
        {
            const auto app = getAppByGUID(guid);
            if (app.isEmpty())
            {
                LOG_WARNING() << "Failed to find Dapp by guid " << guid.toStdString();
                return;
            }

            const auto ipfsID = app["ipfs_id"].toString();
            const auto appName = app["name"].toString();

            // get dapp binary data from ipfs
            QPointer<AppsViewModel> guard(this);
            auto ipfs = AppModel::getInstance().getWalletModel()->getIPFS();

            // TODO: check timeout value
            ipfs->AnyThread_get(ipfsID.toStdString(), 0,
                [this, guard, appName, guid](beam::ByteBuffer&& data) mutable
                {
                    if (!guard)
                    {
                        return;
                    }

                    try
                    {
                        // unpack & verify & install
                        LOG_DEBUG() << "Installing DApp " << appName.toStdString() << " from ipfs";

                        QByteArray qData;
                        std::copy(data.cbegin(), data.cend(), std::back_inserter(qData));

                        QBuffer buffer(&qData);
                        
                        // TODO: does we need add additional verification of the DApp file?

                        installFromBuffer(&buffer, guid);

                        emit appInstallOK(appName);
                        loadApps();
                    }
                    catch (std::runtime_error& err)
                    {
                        LOG_ERROR() << "Failed to install DApp: " << err.what();
                        emit appInstallFail(appName);
                    }
                },
                [this, guard, appName](std::string&& err)
                {
                    LOG_ERROR() << "Failed to get app from ipfs: " << err;
                    emit appInstallFail(appName);
                }
                );
        }
        catch (const std::runtime_error& err)
        {
            assert(false);
            LOG_WARNING() << "Failed to get properties for " << guid.toStdString() << ", " << err.what();
            return;
        }
    }

    void AppsViewModel::installFromBuffer(QIODevice* ioDevice, const QString& guid)
    {
        const auto appsPath = AppSettings().getLocalAppsPath();
        const auto appFolder = QDir(appsPath).filePath(guid);

        if (QDir(appFolder).exists())
        {
            if (!QDir(appFolder).removeRecursively())
            {
                throw std::runtime_error("Failed to prepare folder");
            }
        }

        QDir(appsPath).mkdir(guid);
        if (JlCompress::extractDir(ioDevice, appFolder).isEmpty())
        {
            throw std::runtime_error("DApp Installation failed");
        }
    }

    QString AppsViewModel::installFromFile(const QString& rawFname)
    {
        try
        {
            QString fname = removeFilePrefix(rawFname);

            LOG_DEBUG() << "Installing DApp from file " << rawFname.toStdString() << " | " << fname.toStdString();

            QuaZip zip(fname);
            if(!zip.open(QuaZip::Mode::mdUnzip))
            {
                throw std::runtime_error("Failed to open the DApp file");
            }

            QString guid, appName;
            for (bool ok = zip.goToFirstFile(); ok; ok = zip.goToNextFile())
            {
                const auto zipFname = zip.getCurrentFileName();
                if (zipFname == "manifest.json")
                {
                    QuaZipFile mfile(zip.getZipName(), zipFname);
                    if (!mfile.open(QIODevice::ReadOnly))
                    {
                        throw std::runtime_error("Failed to read the DApp file");
                    }

                    QTextStream in(&mfile);
                    const auto app = parseAppManifest(in, "");
                    guid = app["guid"].value<QString>();
                    appName = app["name"].value<QString>();
                }
            }

            if (guid.isEmpty())
            {
                throw std::runtime_error("Invalid DApp file");
            }

            const auto appsPath = AppSettings().getLocalAppsPath();
            const auto appFolder = QDir(appsPath).filePath(guid);

            if (QDir(appFolder).exists())
            {
                if(!QDir(appFolder).removeRecursively())
                {
                    throw std::runtime_error("Failed to prepare folder");
                }
            }

            QDir(appsPath).mkdir(guid);
            if(JlCompress::extractDir(fname, appFolder).isEmpty())
            {
                //cleanupFolder(appFolder)
                throw std::runtime_error("DApp Installation failed");
            }

            return appName;
        }
        catch(std::exception& err)
        {
            LOG_ERROR() << "Failed to install DApp: " << err.what();
            return "";
        }
    }

    void AppsViewModel::handleShaderTxData(Action action, const beam::ByteBuffer& data)
    {
        try
        {
            beam::bvm2::ContractInvokeData contractInvokeData;

            if (!beam::wallet::fromByteBuffer(data, contractInvokeData))
            {
                throw std::runtime_error("Failed to parse invoke data");
            }

            const auto comment = beam::bvm2::getFullComment(contractInvokeData);
            const auto fee = beam::bvm2::getFullFee(contractInvokeData, AppModel::getInstance().getWalletModel()->getCurrentHeight());
            const auto fullSpend = beam::bvm2::getFullSpend(contractInvokeData);

            if (!fullSpend.empty())
            {
                throw std::runtime_error("Unexpected fullSpend amounts");
            }

            const auto assetsManager = AppModel::getInstance().getAssets();
            const auto feeRate = beamui::AmountToUIString(assetsManager->getRate(beam::Asset::s_BeamID));
            const auto rateUnit = assetsManager->getRateUnit();
            const auto tmp = QString::fromStdString(beam::to_hex(data.data(), data.size()));

            emit shaderTxData(static_cast<int>(action), tmp, QString::fromStdString(comment), beamui::AmountToUIString(fee), feeRate, rateUnit);
        }
        catch (const std::runtime_error& err)
        {
            LOG_WARNING() << "Failed to handle shader TX data: " << err.what();
        }
    }

    void AppsViewModel::contractInfoApproved(int action, const QString& data)
    {
        QPointer<AppsViewModel> guard(this);
        beam::ByteBuffer buffer = beam::from_hex(data.toStdString());

        AppModel::getInstance().getWalletModel()->getAsync()->processShaderTxData(buffer,
            [this, guard, action](const std::string& err, const beam::wallet::TxID& id)
            {
                if (!guard)
                {
                    return;
                }

                if (!err.empty())
                {
                    LOG_WARNING() << "Failed to process shader TX: " << ", " << err;
                }

                if (Action::CreatePublisher == static_cast<Action>(action) || Action::UpdatePublisher == static_cast<Action>(action))
                {
                    if (_txId)
                    {
                        throw std::runtime_error("Unprocessed data - txID isn't empty.");
                    }
                    
                    _txId = id;
                    _action = static_cast<Action>(action);

                    emit showTxIsSent();
                }
                // TODO: check TX status
            }
        );
    }

    void AppsViewModel::contractInfoRejected()
    {
    }

    void AppsViewModel::onTransactionsChanged(
        beam::wallet::ChangeAction action,
        const std::vector<beam::wallet::TxDescription>& transactions)
    {
        if (_txId && action == beam::wallet::ChangeAction::Updated)
        {
            for (auto& tx : transactions)
            {
                if (tx.GetTxID() == *_txId)
                {
                    if (tx.m_status == beam::wallet::TxStatus::Completed || tx.m_status == beam::wallet::TxStatus::Failed)
                    {
                        _txId.reset();
                        loadMyPublisherInfo(true, _action == Action::CreatePublisher);
                        return;
                    }
                }
            }
        }
    }

    QList<QVariantMap> AppsViewModel::getPublisherDApps(const QString& publisherKey)
    {
        QList<QVariantMap> publisherApps;

        std::copy_if(_apps.cbegin(), _apps.cend(), std::back_inserter(publisherApps),
            [publisherKey] (const auto& app) -> bool {
                const auto appFieldsIt = app.find("publisher");
                if (appFieldsIt == app.end())
                {
                    return false;
                }
                // skip local installed own apps by publisher, that didn't be uploaded
                return appFieldsIt->toString() == publisherKey && app.contains("ipfs_id");
            }
        );

        return publisherApps;
    }

    QVariantMap AppsViewModel::getAppByGUID(const QString& guid)
    {
        // find app in _apps by guid
        const auto it = std::find_if(_apps.cbegin(), _apps.cend(),
            [guid](const auto& app) -> bool {
                const auto appFieldsIt = app.find("guid");
                if (appFieldsIt == app.end())
                {
                    // TODO: uncomment when all DApps will have new full list of required fields 
                    // assert(false);
                    return false;
                }
                return appFieldsIt->toString() == guid;
            });

        if (it == _apps.end())
        {
            assert(false);
            return {};
        }
        return *it;
    }

    void AppsViewModel::removeDApp(const QString& guid)
    {
        // TODO: change the order of operations to: first remove from contract -> unpin from IPFS
        try
        {
            const auto app = getAppByGUID(guid);
            if (app.isEmpty())
            {
                LOG_WARNING() << "Failed to find Dapp by guid " << guid.toStdString();
                return;
            }

            const auto ipfsID = app["ipfs_id"].toString();
            const auto appName = app["name"].toString();

            // unpin dapp binary data from ipfs
            QPointer<AppsViewModel> guard(this);
            auto ipfs = AppModel::getInstance().getWalletModel()->getIPFS();

            ipfs->AnyThread_unpin(ipfsID.toStdString(),
                [this, guard, appName, guid, ipfsID]() mutable
                {
                    if (!guard)
                    {
                        return;
                    }
                    LOG_INFO() << "Successfully unpin app" << appName.toStdString() << "(" << ipfsID.toStdString() << ") from ipfs : ";
                    deleteAppFromStore(guid);
                },
                [this, guard, appName, ipfsID, guid](std::string&& err)
                {
                    LOG_ERROR() << "Failed to unpin app" << appName.toStdString() << "(" << ipfsID.toStdString() << ") from ipfs : " << err;
                    // TODO: emit appRemoveFail(appName);
                }
            );
        }
        catch (const std::runtime_error& err)
        {
            assert(false);
            LOG_WARNING() << "Failed to get properties for " << guid.toStdString() << ", " << err.what();
            return;
        }
    }

    void AppsViewModel::deleteAppFromStore(const QString& guid)
    {
        std::stringstream argsStream;
        argsStream << "action=delete_dapp,";
        argsStream << "cid=" << AppSettings().getDappStoreCID().c_str();
        argsStream << ",id=" << guid.toStdString();

        QPointer<AppsViewModel> guard(this);
        AppModel::getInstance().getWalletModel()->getAsync()->callShader(AppSettings().getDappStorePath(), argsStream.str(),
            [this, guard](const std::string& err, const std::string& output, const beam::ByteBuffer& data)
            {
                if (!guard)
                {
                    return;
                }

                if (!err.empty())
                {
                    LOG_WARNING() << "Failed to delete app" << ", " << err;
                    return;
                }

                if (data.empty())
                {
                    LOG_WARNING() << "Failed to delete app" << ", " << output;
                    return;
                }
                handleShaderTxData(Action::DeleteDApp, data);
            }
        );
    }
}
