/**
 * Copyright 2013 Albert Vaca <albertvaka@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "device.h"

#ifdef interface // MSVC language extension, QDBusConnection uses this as a variable name
#undef interface
#endif

#include <QDBusConnection>
#include <QFile>

#include <KSharedConfig>
#include <KConfigGroup>
#include <KStandardDirs>
#include <KPluginSelector>
#include <KServiceTypeTrader>
#include <KNotification>
#include <KIcon>

#include "kdebugnamespace.h"
#include "kdeconnectplugin.h"
#include "pluginloader.h"
#include "backends/devicelink.h"
#include "backends/linkprovider.h"
#include "networkpackage.h"

Device::Device(QObject* parent, const QString& id)
    : QObject(parent)
    , m_deviceId(id)
    , m_pairStatus(Device::Paired)
    , m_protocolVersion(NetworkPackage::ProtocolVersion) //We don't know it yet
{
    KSharedConfigPtr config = KSharedConfig::openConfig("kdeconnectrc");
    KConfigGroup data = config->group("trusted_devices").group(id);

    m_deviceName = data.readEntry<QString>("deviceName", QLatin1String("unnamed"));
    m_deviceType = str2type(data.readEntry<QString>("deviceType", QLatin1String("unknown")));

    const QString& key = data.readEntry<QString>("publicKey", QString());
    m_publicKey = QCA::RSAPublicKey::fromPEM(key);
    
    initPrivateKey();

    //Register in bus
    QDBusConnection::sessionBus().registerObject(dbusPath(), this, QDBusConnection::ExportScriptableContents | QDBusConnection::ExportAdaptors);
}

Device::Device(QObject* parent, const NetworkPackage& identityPackage, DeviceLink* dl)
    : QObject(parent)
    , m_deviceId(identityPackage.get<QString>("deviceId"))
    , m_deviceName(identityPackage.get<QString>("deviceName"))
    , m_deviceType(str2type(identityPackage.get<QString>("deviceType")))
    , m_pairStatus(Device::NotPaired)
    , m_protocolVersion(identityPackage.get<int>("protocolVersion"))
    , m_incomingCapabilities(identityPackage.get<QStringList>("SupportedIncomingInterfaces", QStringList()).toSet())
    , m_outgoingCapabilities(identityPackage.get<QStringList>("SupportedOutgoingInterfaces", QStringList()).toSet())
{
    initPrivateKey();

    addLink(identityPackage, dl);
    
    //Register in bus
    QDBusConnection::sessionBus().registerObject(dbusPath(), this, QDBusConnection::ExportScriptableContents | QDBusConnection::ExportAdaptors);
}

void Device::initPrivateKey()
{
    //TODO: It is redundant to have our own private key in every instance of Device, move this to a singleton somewhere (Daemon?)
    const QString privateKeyPath = KStandardDirs::locateLocal("appdata", "key.pem", true, KComponentData("kdeconnect", "kdeconnect"));
    QFile privKey(privateKeyPath);
    privKey.open(QIODevice::ReadOnly);
    m_privateKey = QCA::PrivateKey::fromPEM(privKey.readAll());
}

Device::~Device()
{

}

bool Device::hasPlugin(const QString& name) const
{
    return m_plugins.contains(name);
}

QStringList Device::loadedPlugins() const
{
    return m_plugins.keys();
}

void Device::reloadPlugins()
{
    QMap<QString, KdeConnectPlugin*> newPluginMap;
    QMultiMap<QString, KdeConnectPlugin*> newPluginsByIncomingInterface;
    QMultiMap<QString, KdeConnectPlugin*> newPluginsByOutgoingInterface;

    if (isPaired() && isReachable()) { //Do not load any plugin for unpaired devices, nor useless loading them for unreachable devices

        QString path = KGlobal::dirs()->findResource("config", "kdeconnect/"+id());
        KConfigGroup pluginStates = KSharedConfig::openConfig(path)->group("Plugins");

        PluginLoader* loader = PluginLoader::instance();

        //Code borrowed from KWin
        foreach (const QString& pluginName, loader->getPluginList()) {
            QString enabledKey = pluginName + QString::fromLatin1("Enabled");

            bool isPluginEnabled = (pluginStates.hasKey(enabledKey) ? pluginStates.readEntry(enabledKey, false)
                                                            : loader->getPluginInfo(pluginName).isPluginEnabledByDefault());

            if (isPluginEnabled) {
                KdeConnectPlugin* plugin = m_plugins.take(pluginName);
                QStringList incomingInterfaces, outgoingInterfaces;
                if (plugin) {
                    incomingInterfaces = m_pluginsByIncomingInterface.keys(plugin);
                    outgoingInterfaces = m_pluginsByOutgoingInterface.keys(plugin);
                } else {
                    KService::Ptr service = loader->pluginService(pluginName);
                    incomingInterfaces = service->property("X-KdeConnect-SupportedPackageType", QVariant::StringList).toStringList();
                    outgoingInterfaces = service->property("X-KdeConnect-OutgoingPackageType", QVariant::StringList).toStringList();
                }

                //If we don't find intersection with the received on one end and the sent on the other, we don't
                //let the plugin stay
                //Also, if no capabilities are specified on the other end, we don't apply this optimizaton, as
                //we asume that the other client doesn't know about capabilities.
                if (!m_incomingCapabilities.isEmpty() && !m_outgoingCapabilities.isEmpty()
                    && m_incomingCapabilities.intersect(outgoingInterfaces.toSet()).isEmpty()
                    && m_outgoingCapabilities.intersect(incomingInterfaces.toSet()).isEmpty()
                ) {
                    delete plugin;
                    continue;
                }

                if (!plugin) {
                    plugin = loader->instantiatePluginForDevice(pluginName, this);
                }

                foreach(const QString& interface, incomingInterfaces) {
                    newPluginsByIncomingInterface.insert(interface, plugin);
                }
                foreach(const QString& interface, outgoingInterfaces) {
                    newPluginsByOutgoingInterface.insert(interface, plugin);
                }
                newPluginMap[pluginName] = plugin;
            }
        }
    }

    //Erase all left plugins in the original map (meaning that we don't want
    //them anymore, otherwise they would have been moved to the newPluginMap)
    qDeleteAll(m_plugins);
    m_plugins = newPluginMap;
    m_pluginsByIncomingInterface = newPluginsByIncomingInterface;
    m_pluginsByOutgoingInterface = newPluginsByOutgoingInterface;

    Q_FOREACH(KdeConnectPlugin* plugin, m_plugins) {
        plugin->connected();
    }

    Q_EMIT pluginsChanged();

}

void Device::requestPair()
{
    switch(m_pairStatus) {
        case Device::Paired:
            Q_EMIT pairingFailed(i18n("Already paired"));
            return;
        case Device::Requested:
            Q_EMIT pairingFailed(i18n("Pairing already requested for this device"));
            return;
        default:
            if (!isReachable()) {
                Q_EMIT pairingFailed(i18n("Device not reachable"));
                return;
            }
            break;
    }

    m_pairStatus = Device::Requested;

    //Send our own public key
    bool success = sendOwnPublicKey();

    if (!success) {
        m_pairStatus = Device::NotPaired;
        Q_EMIT pairingFailed(i18n("Error contacting device"));
        return;
    }

    if (m_pairStatus == Device::Paired) {
        return;
    }

    m_pairingTimeut.setSingleShot(true);
    m_pairingTimeut.start(30 * 1000); //30 seconds of timeout
    connect(&m_pairingTimeut, SIGNAL(timeout()),
            this, SLOT(pairingTimeout()));

}

void Device::unpair()
{
    m_pairStatus = Device::NotPaired;

    KSharedConfigPtr config = KSharedConfig::openConfig("kdeconnectrc");
    config->group("trusted_devices").deleteGroup(id());

    NetworkPackage np(PACKAGE_TYPE_PAIR);
    np.set("pair", false);
    sendPackage(np);

    reloadPlugins(); //Will unload the plugins

    Q_EMIT unpaired();

}

void Device::pairingTimeout()
{
    NetworkPackage np(PACKAGE_TYPE_PAIR);
    np.set("pair", false);
    sendPackage(np);
    m_pairStatus = Device::NotPaired;
    Q_EMIT pairingFailed(i18n("Timed out"));
}

static bool lessThan(DeviceLink* p1, DeviceLink* p2)
{
    return p1->provider()->priority() > p2->provider()->priority();
}

void Device::addLink(const NetworkPackage& identityPackage, DeviceLink* link)
{
    //kDebug(debugArea()) << "Adding link to" << id() << "via" << link->provider();

    m_protocolVersion = identityPackage.get<int>("protocolVersion");
    if (m_protocolVersion != NetworkPackage::ProtocolVersion) {
        qWarning() << m_deviceName << "- warning, device uses a different protocol version" << m_protocolVersion << "expected" << NetworkPackage::ProtocolVersion;
    }

    connect(link, SIGNAL(destroyed(QObject*)),
            this, SLOT(linkDestroyed(QObject*)));

    m_deviceLinks.append(link);

    //re-read the device name from the identityPackage because it could have changed
    m_deviceName = identityPackage.get<QString>("deviceName");
    m_deviceType = str2type(identityPackage.get<QString>("deviceType"));

    Q_ASSERT(!m_privateKey.isNull());
    link->setPrivateKey(m_privateKey);

    //Theoretically we will never add two links from the same provider (the provider should destroy
    //the old one before this is called), so we do not have to worry about destroying old links.
    //Actually, we should not destroy them or the provider will store an invalid ref!

    connect(link, SIGNAL(receivedPackage(NetworkPackage)),
            this, SLOT(privateReceivedPackage(NetworkPackage)));

    qSort(m_deviceLinks.begin(), m_deviceLinks.end(), lessThan);

    if (m_deviceLinks.size() == 1) {
        reloadPlugins(); //Will load the plugins
        Q_EMIT reachableStatusChanged();
    } else {
        Q_FOREACH(KdeConnectPlugin* plugin, m_plugins) {
            plugin->connected();
        }
    }
}

void Device::linkDestroyed(QObject* o)
{
    removeLink(static_cast<DeviceLink*>(o));
}

void Device::removeLink(DeviceLink* link)
{
    m_deviceLinks.removeOne(link);

    //kDebug(debugArea()) << "RemoveLink" << m_deviceLinks.size() << "links remaining";

    if (m_deviceLinks.isEmpty()) {
        reloadPlugins();
        Q_EMIT reachableStatusChanged();
    }
}

QString Device::privateKeyPath() const
{
    return KSharedConfig::openConfig("kdeconnectrc")->group("myself").readEntry("privateKeyPath");
}

bool Device::sendPackage(NetworkPackage& np)
{
    if (np.type() != PACKAGE_TYPE_PAIR && isPaired()) {
        Q_FOREACH(DeviceLink* dl, m_deviceLinks) {
            if (dl->sendPackageEncrypted(m_publicKey, np)) return true;
        }
    } else {
        //Maybe we could block here any package that is not an identity or a pairing package to prevent sending non encrypted data
        Q_FOREACH(DeviceLink* dl, m_deviceLinks) {
            if (dl->sendPackage(np)) return true;
        }
    }

    return false;
}

void Device::privateReceivedPackage(const NetworkPackage& np)
{
    if (np.type() == PACKAGE_TYPE_PAIR) {

        //kDebug(debugArea()) << "Pair package";

        bool wantsPair = np.get<bool>("pair");

        if (wantsPair == isPaired()) {
            kDebug(debugArea()) << "Already" << (wantsPair? "paired":"unpaired");
            if (m_pairStatus == Device::Requested) {
                m_pairStatus = Device::NotPaired;
                m_pairingTimeut.stop();
                Q_EMIT pairingFailed(i18n("Canceled by other peer"));
            }
            return;
        }

        if (wantsPair) {

            //Retrieve their public key
            const QString& key = np.get<QString>("publicKey");
            m_publicKey = QCA::RSAPublicKey::fromPEM(key);
            if (m_publicKey.isNull()) {
                kDebug(debugArea()) << "ERROR decoding key";
                if (m_pairStatus == Device::Requested) {
                    m_pairStatus = Device::NotPaired;
                    m_pairingTimeut.stop();
                }
                Q_EMIT pairingFailed(i18n("Received incorrect key"));
                return;
            }

            if (m_pairStatus == Device::Requested)  { //We started pairing

                kDebug(debugArea()) << "Pair answer";
                setAsPaired();

            } else {

                kDebug(debugArea()) << "Pair request";

                KNotification* notification = new KNotification("pairingRequest");
                notification->setPixmap(KIcon("dialog-information").pixmap(48, 48));
                notification->setComponentData(KComponentData("kdeconnect", "kdeconnect"));
                notification->setTitle("KDE Connect");
                notification->setText(i18n("Pairing request from %1", m_deviceName));
                notification->setActions(QStringList() << i18n("Accept") << i18n("Reject"));
                connect(notification, SIGNAL(action1Activated()), this, SLOT(acceptPairing()));
                connect(notification, SIGNAL(action2Activated()), this, SLOT(rejectPairing()));
                notification->sendEvent();

                m_pairStatus = Device::RequestedByPeer;

            }

        } else {

            kDebug(debugArea()) << "Unpair request";

            PairStatus prevPairStatus = m_pairStatus;
            m_pairStatus = Device::NotPaired;

            if (prevPairStatus == Device::Requested) {
                m_pairingTimeut.stop();
                Q_EMIT pairingFailed(i18n("Canceled by other peer"));
            } else if (prevPairStatus == Device::Paired) {
                KSharedConfigPtr config = KSharedConfig::openConfig("kdeconnectrc");
                config->group("trusted_devices").deleteGroup(id());
                reloadPlugins();
                Q_EMIT unpaired();
            }

        }

    } else if (isPaired()) {
        QList<KdeConnectPlugin*> plugins = m_pluginsByIncomingInterface.values(np.type());
        foreach(KdeConnectPlugin* plugin, plugins) {
            plugin->receivePackage(np);
        }
    } else {
        kDebug(debugArea()) << "device" << name() << "not paired, ignoring package" << np.type();
        unpair();

    }

}

bool Device::sendOwnPublicKey()
{
    NetworkPackage np(PACKAGE_TYPE_PAIR);
    np.set("pair", true);
    np.set("publicKey", m_privateKey.toPublicKey().toPEM());
    bool success = sendPackage(np);
    return success;
}

void Device::rejectPairing()
{
    kDebug(debugArea()) << "Rejected pairing";

    m_pairStatus = Device::NotPaired;

    NetworkPackage np(PACKAGE_TYPE_PAIR);
    np.set("pair", false);
    sendPackage(np);

    Q_EMIT pairingFailed(i18n("Canceled by the user"));

}

void Device::acceptPairing()
{
    if (m_pairStatus != Device::RequestedByPeer) return;

    kDebug(debugArea()) << "Accepted pairing";

    bool success = sendOwnPublicKey();

    if (!success) {
        m_pairStatus = Device::NotPaired;
        return;
    }

    setAsPaired();

}

void Device::setAsPaired()
{

    m_pairStatus = Device::Paired;

    m_pairingTimeut.stop(); //Just in case it was started

    storeAsTrusted(); //Save to the config as trusted

    reloadPlugins(); //Will actually load the plugins

    Q_EMIT pairingSuccesful();

}

void Device::storeAsTrusted()
{
    KSharedConfigPtr config = KSharedConfig::openConfig("kdeconnectrc");
    config->group("trusted_devices").group(id()).writeEntry("publicKey", m_publicKey.toPEM());
    config->group("trusted_devices").group(id()).writeEntry("deviceName", name());
    config->group("trusted_devices").group(id()).writeEntry("deviceType", type2str(m_deviceType));
    config->sync();
}

QStringList Device::availableLinks() const
{
    QStringList sl;
    Q_FOREACH(DeviceLink* dl, m_deviceLinks) {
        sl.append(dl->provider()->name());
    }
    return sl;
}

Device::DeviceType Device::str2type(QString deviceType) {
    if (deviceType == "desktop") return Desktop;
    if (deviceType == "laptop") return Laptop;
    if (deviceType == "phone") return Phone;
    if (deviceType == "tablet") return Tablet;
    return Unknown;
}

QString Device::type2str(Device::DeviceType deviceType) {
    if (deviceType == Desktop) return "desktop";
    if (deviceType == Laptop) return "laptop";
    if (deviceType == Phone) return "phone";
    if (deviceType == Tablet) return "tablet";
    return "unknown";
}

QString Device::iconName() const
{
    switch(m_deviceType) {
        case Device::Desktop:
            return "computer";
        case Device::Laptop:
            return "computer-laptop";
        case Device::Phone:
            return "smartphone";
        case Device::Tablet:
            return "tablet";
        case Device::Unknown:
            return "unknown";
    }
    return QString();
}
