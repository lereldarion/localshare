#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <QHostInfo>
#include <QSocketNotifier>
#include <QString>
#include <QTime>
#include <QtEndian>

#include <dns_sd.h>
#include <utility> // std::forward

#include "compatibility.h"
#include "localshare.h"
#include "settings.h"

namespace Discovery {
/* Service name vs Username.
 *
 * Username is what is printed in the Gui and chosen by the user.
 * By default it is the user session name.
 *
 * A service name must be unique on the local network.
 * It is built by adding a '@'+suffix to the username.
 * This allows to have two computers with the username on the network.
 */
inline QString username_of (const QString & service_name) {
	auto s = service_name.section ('@', 0, -2);
	if (!s.isEmpty ())
		return s;
	return service_name; // Fallback if not compliant (or <v0.3)
}
inline QString service_name_of (const QString & username, const QString & suffix) {
	return QString ("%1@%2").arg (username, suffix);
}

/* QObject representing a discovered peer.
 * These objects are generated by the browser.
 * They are destroyed when the peer disappear.
 * They are owned by the browser, and will die with it.
 *
 * The service name is constant after discovery.
 * Other discovered information (hostname, port) send notify signals if updated.
 */
class DnsPeer : public QObject {
	Q_OBJECT

private:
	const QString service_name;
	QString hostname;
	quint16 port; // Host byte order

signals:
	void hostname_changed (void);
	void port_changed (void);

public:
	DnsPeer (const QString & service_name, QObject * parent = nullptr)
	    : QObject (parent), service_name (service_name) {}

	QString get_service_name (void) const { return service_name; }
	QString get_username (void) const { return username_of (service_name); }
	QString get_hostname (void) const { return hostname; }
	void set_hostname (const QString & new_hostname) {
		if (hostname != new_hostname) {
			hostname = new_hostname;
			emit hostname_changed ();
		}
	}
	quint16 get_port (void) const { return port; }
	void set_port (quint16 new_port) {
		if (port != new_port) {
			port = new_port;
			emit port_changed ();
		}
	}
};

/* LocalDnsPeer represents the local instance of localshare.
 *
 * service_name is the current service name.
 * If modified (
 */
class LocalDnsPeer : public QObject {
	Q_OBJECT

private:
	QString suffix;
	Settings::Username requested_username;
	QString service_name;
	const quint16 port; // Host byte order

signals:
	void requested_service_name_changed (void);
	void username_changed (void);
	void service_name_changed (void);

public:
	LocalDnsPeer (quint16 server_port, QObject * parent = nullptr)
	    : QObject (parent), port (server_port) {
		// Suffix is hostname, or a random number stringified if not available
		suffix = QHostInfo::localHostName ();
		if (suffix.isEmpty ()) {
			qsrand (QTime::currentTime ().msec ());
			suffix.setNum (qrand ());
		}
	}

	QString get_requested_username (void) { return requested_username.get (); }
	QString get_requested_service_name (void) {
		return service_name_of (get_requested_username (), suffix);
	}
	void set_requested_username (const QString & new_username) {
		if (get_requested_username () != new_username) {
			requested_username.set (new_username);
			emit requested_service_name_changed (); // Guaranteed to be changed
		}
	}

	QString get_service_name (void) const { return service_name; }
	QString get_username (void) const { return username_of (service_name); }
	void set_service_name (const QString & new_service_name) {
		auto old_username = get_username ();
		if (service_name != new_service_name) {
			service_name = new_service_name;
			emit service_name_changed ();
		}
		if (old_username != get_username ())
			emit username_changed (); // May not change if only suffix is changed
	}

	quint16 get_port (void) const { return port; }
};

/* Helper class to manage a DNSServiceRef.
 * This class must be dynamically allocated.
 */
class DnsSocket : public QObject {
	Q_OBJECT

private:
	DNSServiceRef ref{nullptr};
	QString error_msg;

signals:
	/* On delete this class will send this signal.
	 * When sent, it is guaranteed that the service has been closed.
	 * The string "error" may be empty if it is a normal termination.
	 */
	void being_destroyed (QString error);

public:
	DnsSocket (QObject * parent = nullptr) : QObject (parent) {}
	~DnsSocket () {
		DNSServiceRefDeallocate (ref);
		emit being_destroyed (error_msg);
	}

protected:
	template <typename QueryFunc, typename... Args>
	void init_with (QueryFunc && query_func, Args &&... args) {
		auto err = std::forward<QueryFunc> (query_func) (&ref, std::forward<Args> (args)...);
		if (has_error (err)) {
			failure (err);
			return;
		}
		auto fd = DNSServiceRefSockFD (ref);
		if (fd != -1) {
			auto notifier = new QSocketNotifier (fd, QSocketNotifier::Read, this);
			connect (notifier, &QSocketNotifier::activated, this, &DnsSocket::has_pending_data);
		} else {
			// Should never happen, the function is just an accessor
			qFatal ("DNSServiceRefSockFD failed");
		}
	}

	// Error handling
	using Error = DNSServiceErrorType;
	static bool has_error (Error e) { return e != kDNSServiceErr_NoError; }

	void failure (Error e) {
		Q_ASSERT (error_msg.isEmpty ()); // Should only be called once
		error_msg = make_error_string (e);
		deleteLater ();
	}

	virtual QString make_error_string (Error e) const {
		switch (e) {
		case kDNSServiceErr_NoError:
			return tr ("No error");
		case kDNSServiceErr_Unknown:
			return tr ("Unknown error");
		case kDNSServiceErr_NoSuchName:
			return tr ("Internal error: No such name");
		case kDNSServiceErr_NoMemory:
			return tr ("Out of memory");
		case kDNSServiceErr_BadParam:
			return tr ("API error: Bad parameter");
		case kDNSServiceErr_BadReference:
			return tr ("API error: Bad DNSServiceRef");
		case kDNSServiceErr_BadState:
			return tr ("Internal error: Bad state");
		case kDNSServiceErr_BadFlags:
			return tr ("API error: Bad flags");
		case kDNSServiceErr_Unsupported:
			return tr ("Unsupported operation");
		case kDNSServiceErr_NotInitialized:
			return tr ("API error: DNSServiceRef is not initialized");
		case kDNSServiceErr_AlreadyRegistered:
			return tr ("Service is already registered");
		case kDNSServiceErr_NameConflict:
			return tr ("Service name is already taken");
		case kDNSServiceErr_Invalid:
			return tr ("API error: Invalid data");
		case kDNSServiceErr_Firewall:
			return tr ("Firewall");
		case kDNSServiceErr_Incompatible:
			return tr ("Localshare incompatible with local Zeroconf service");
		case kDNSServiceErr_BadInterfaceIndex:
			return tr ("API error: Bad interface index");
		case kDNSServiceErr_Refused:
			return tr ("kDNSServiceErr_Refused");
		case kDNSServiceErr_NoSuchRecord:
			return tr ("kDNSServiceErr_NoSuchRecord");
		case kDNSServiceErr_NoAuth:
			return tr ("kDNSServiceErr_NoAuth");
		case kDNSServiceErr_NoSuchKey:
			return tr ("The key does not exist in the TXT record");
		case kDNSServiceErr_NATTraversal:
			return tr ("kDNSServiceErr_NATTraversal");
		case kDNSServiceErr_DoubleNAT:
			return tr ("kDNSServiceErr_DoubleNAT");
		case kDNSServiceErr_BadTime:
			return tr ("kDNSServiceErr_BadTime");
		case -65563: // kDNSServiceErr_ServiceNotRunning, only in recent versions
			return tr ("Zeroconf service in not running");
		default:
			return tr ("Unknown error code: %1").arg (e);
		}
	}

private slots:
	void has_pending_data (void) {
		auto err = DNSServiceProcessResult (ref);
		if (has_error (err))
			failure (err);
	}
};

/* Service registration class.
 *
 * Registers at construction, and unregister at destruction.
 * Name of service is limited to kDNSServiceMaxServiceName utf8 bytes.
 * Longer names will be truncated by the library, so do nothing about that.
 *
 * It tries to register the local peer desired_name.
 * The returned name may be different, and will be provided to the local_peer by set_name.
 * This may trigger updates to other objects following the local_peer information.
 */
class ServiceRecord : public DnsSocket {
	Q_OBJECT

public:
	ServiceRecord (LocalDnsPeer * local_peer) : DnsSocket (local_peer) {
		auto name = local_peer->get_requested_service_name ();
		qDebug ("ServiceRecord[%p]: registering \"%s\"", this, qPrintable (name));
		init_with (DNSServiceRegister, 0 /* flags */, 0 /* any interface */, qUtf8Printable (name),
		           qUtf8Printable (Const::service_type), nullptr /* default domain */,
		           nullptr /* default hostname */,
		           qToBigEndian (local_peer->get_port ()) /* port in NBO */, 0,
		           nullptr /* text len and text */, register_callback, this /* context */);
	}
	~ServiceRecord () {
		qDebug ("ServiceRecord[%p]: shutting down", this);
		if (auto lp = get_local_peer ())
			lp->set_service_name (QString ());
	}

private:
	static void DNSSD_API register_callback (DNSServiceRef, DNSServiceFlags,
	                                         DNSServiceErrorType error_code,
	                                         const char * service_name, const char * /* regtype */,
	                                         const char * /* domain */, void * context) {
		auto c = static_cast<ServiceRecord *> (context);
		if (has_error (error_code)) {
			c->failure (error_code);
			return;
		}
		qDebug ("ServiceRecord[%p]: registered \"%s\"", c, service_name);
		c->get_local_peer ()->set_service_name (service_name);
	}

	QString make_error_string (Error e) const Q_DECL_OVERRIDE {
		return tr ("Service registration failed: %1").arg (DnsSocket::make_error_string (e));
	}

	LocalDnsPeer * get_local_peer (void) { return qobject_cast<LocalDnsPeer *> (parent ()); }
};

/* Temporary structure use to represent a resolve query.
 * Resolve query : from service_name id to hostname, port.
 *
 * A DnsPeer object is created empty, and owned by the Query.
 * When finally filled, it is published.
 * The browser should receive the reference and get ownership of the DnsPeer.
 * If the query fails or succeeds, it then deletes itself.
 * If the DnsPeer was not taken by the Browser, it will be deleted too.
 */
class Resolver : public DnsSocket {
	Q_OBJECT

private:
	DnsPeer * peer;

signals:
	void peer_resolved (DnsPeer *);

public:
	Resolver (uint32_t interface_index, const char * service_name, const char * regtype,
	          const char * domain, QObject * parent = nullptr)
	    : DnsSocket (parent) {
		peer = new DnsPeer (service_name, this);
		init_with (DNSServiceResolve, 0 /* flags */, interface_index, service_name, regtype, domain,
		           resolver_callback, this /* context */);
	}

private:
	static void DNSSD_API resolver_callback (DNSServiceRef, DNSServiceFlags, uint32_t /* interface */,
	                                         DNSServiceErrorType error_code,
	                                         const char * /* fullname */, const char * hostname,
	                                         uint16_t port, uint16_t /* txt len */,
	                                         const unsigned char * /* txt record */, void * context) {
		auto c = static_cast<Resolver *> (context);
		if (has_error (error_code)) {
			c->failure (error_code);
			return;
		}
		c->peer->set_port (qFromBigEndian (port)); // To host byte order
		c->peer->set_hostname (hostname);
		emit c->peer_resolved (c->peer);
		c->deleteLater ();
	}

	QString make_error_string (Error e) const Q_DECL_OVERRIDE {
		return tr ("Service resolver failed: %1").arg (DnsSocket::make_error_string (e));
	}
};

/* Browser object.
 * Starts browsing at creation, stops at destruction.
 * Emits added/removed signals to indicate changes to peer list.
 *
 * It owns DnsPeer objects representing discovered peers.
 * DnsPeer objects will be destroyed when the peer disappears.
 * They are all destroyed when the Browser dies.
 *
 * The local_peer username might be changed ; currently nothing reacts to it.
 * The filtering test will use the new name without care.
 */
class Browser : public DnsSocket {
	Q_OBJECT

signals:
	void added (DnsPeer *);

public:
	Browser (LocalDnsPeer * local_peer) : DnsSocket (local_peer) {
		connect (local_peer, &LocalDnsPeer::service_name_changed, this, &Browser::service_name_changed);
		qDebug ("Browser[%p]: started", this);
		init_with (DNSServiceBrowse, 0 /* flags */, 0 /* interface */,
		           qUtf8Printable (Const::service_type), nullptr /* default domain */, browser_callback,
		           this /* context */);
	}
	~Browser () { qDebug ("Browser[%p]: shutting down", this); }

private:
	static void DNSSD_API browser_callback (DNSServiceRef, DNSServiceFlags flags,
	                                        uint32_t interface_index, DNSServiceErrorType error_code,
	                                        const char * service_name, const char * regtype,
	                                        const char * domain, void * context) {
		auto c = static_cast<Browser *> (context);
		if (has_error (error_code)) {
			c->failure (error_code);
			return;
		}
		if (flags & kDNSServiceFlagsAdd) {
			// Peer is added, find its contact info
			auto r = new Resolver (interface_index, service_name, regtype, domain, c);
			connect (r, &Resolver::peer_resolved, c, &Browser::peer_resolved);
			connect (r, &Resolver::being_destroyed, [=](const QString & msg) {
				if (!msg.isEmpty ())
					qWarning ("Browser[%p]: Resolver failure: %s", c, qPrintable (msg));
			});
		} else {
			// Peer is removed
			if (auto p = c->find_peer_by_service_name (service_name))
				p->deleteLater ();
		}
	}

private slots:
	void peer_resolved (DnsPeer * peer) {
		if (auto p = find_peer_by_service_name (peer->get_service_name ())) {
			// Update, and let peer be discarded
			qDebug ("Browser[%p]: updating \"%s\"", this, qPrintable (peer->get_service_name ()));
			p->set_hostname (peer->get_hostname ());
			p->set_port (peer->get_port ());
		} else {
			// Add and take ownership
			if (get_local_peer ()->get_service_name () != peer->get_service_name ()) {
				qDebug ("Browser[%p]: adding \"%s\"", this, qPrintable (peer->get_service_name ()));
				peer->setParent (this);
				emit added (peer);
			} else {
				qDebug ("Browser[%p]: ignoring \"%s\"", this, qPrintable (peer->get_service_name ()));
			}
		}
	}

	void service_name_changed (void) {
		// Stop tracking our own service record
		if (auto p = find_peer_by_service_name (get_local_peer ()->get_service_name ()))
			p->deleteLater ();
	}

private:
	DnsPeer * find_peer_by_service_name (const QString & service_name) {
		for (auto dns_peer : findChildren<DnsPeer *> (QString (), Qt::FindDirectChildrenOnly))
			if (dns_peer->get_service_name () == service_name)
				return dns_peer;
		return nullptr;
	}

	QString make_error_string (Error e) const Q_DECL_OVERRIDE {
		return tr ("Service browser failed: %1").arg (DnsSocket::make_error_string (e));
	}

	LocalDnsPeer * get_local_peer (void) { return qobject_cast<LocalDnsPeer *> (parent ()); }
};
}

#endif
