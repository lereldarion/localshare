#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <QHostInfo>
#include <QSet>
#include <QSocketNotifier>
#include <QtEndian>

#include <dns_sd.h>
#include <utility> // std::forward

#include "compatibility.h"
#include "localshare.h"

namespace Discovery {

class Service;
class Browser;

/*
 * Helper class to manage a DNSServiceRef
 */
class DnsSocket : public QObject {
	Q_OBJECT

private:
	DNSServiceRef ref;

signals:
	void error (DNSServiceErrorType);

public:
	DnsSocket (const DNSServiceRef & ref, QObject * parent = nullptr) : QObject (parent), ref (ref) {
		auto fd = DNSServiceRefSockFD (ref);
		if (fd != -1) {
			auto notifier = new QSocketNotifier (fd, QSocketNotifier::Read, this);
			connect (notifier, &QSocketNotifier::activated, this, &DnsSocket::has_pending_data);
		} else {
			qFatal ("DNSServiceRefSockFD failed");
		}
	}
	~DnsSocket () { DNSServiceRefDeallocate (ref); }

	template <typename... Args> static DnsSocket * fromRegister (QObject * parent, Args &&... args) {
		return fromFunc (parent, DNSServiceRegister, "DNSServiceRegister",
		                 std::forward<Args> (args)...);
	}
	template <typename... Args> static DnsSocket * fromResolve (QObject * parent, Args &&... args) {
		return fromFunc (parent, DNSServiceResolve, "DNSServiceResolve", std::forward<Args> (args)...);
	}
	template <typename... Args> static DnsSocket * fromBrowse (QObject * parent, Args &&... args) {
		return fromFunc (parent, DNSServiceBrowse, "DNSServiceBrowse", std::forward<Args> (args)...);
	}

private:
	template <typename QueryFunc, typename... Args>
	static DnsSocket * fromFunc (QObject * parent, QueryFunc && query_func, const char * func_name,
	                             Args &&... args) {
		DNSServiceRef ref;
		auto err = std::forward<QueryFunc> (query_func) (&ref, std::forward<Args> (args)...);
		if (err == kDNSServiceErr_NoError) {
			return new DnsSocket (ref, parent);
		} else {
			qWarning () << func_name << "error:" << err;
			return nullptr;
		}
	}

private slots:
	void has_pending_data (void) {
		auto err = DNSServiceProcessResult (ref);
		if (err != kDNSServiceErr_NoError) {
			qWarning () << "DNSServiceProcessResult error:" << err;
			emit error (err);
		}
	}
};

/*
 * Service announce class
 */
class Service : public QObject {
	Q_OBJECT

	/* Registers at construction, and unregister at destruction.
	 * Name of service is limited to kDNSServiceMaxServiceName utf8 bytes (64B including '\0'
	 * currently). Longer names will be truncated by the library, so do nothing about that. The actual
	 * registered name will be provided by the callback and given to the app through the registered
	 * signal.
	 *
	 * Errors are critical.
	 * TODO expose them and handle them ?
	 */
signals:
	void registered (QString name);

public:
	Service (const QString & name, const QString & service_name, quint16 tcp_port,
	         QObject * parent = nullptr)
	    : QObject (parent) {
		DnsSocket::fromRegister (
		    this, 0 /* flags */, 0 /* any interface */, qUtf8Printable (name),
		    qUtf8Printable (service_name), nullptr /* default domain */, nullptr /* default hostname */,
		    qToBigEndian (tcp_port) /* port in NBO */, 0, nullptr /* text len and text */,
		    register_callback, this /* context */);
	}

private:
	static void DNSSD_API register_callback (DNSServiceRef, DNSServiceFlags,
	                                         DNSServiceErrorType error_code, const char * name,
	                                         const char * /* service_type */,
	                                         const char * /* domain */, void * context) {
		auto c = static_cast<Service *> (context);
		if (error_code == kDNSServiceErr_NoError) {
			emit c->registered (name);
		} else {
			qCritical () << "DNSServiceRegister error [callback]:" << error_code;
		}
	}
};

// Name resolver query (internal)
class Resolver : public QObject {
	Q_OBJECT
	/* Temporary structure use to represent a resolve query.
	 * Resolve query : from DNS_SD service id to address, name, port.
	 *
	 * Instances of this class are generated by a parent Browser.
	 * They are child of the browser, and will delete themselves after use (or error), or be deleted
	 * by the Browser if it dies.
	 * At success, they trigger a slot in the Browser.
	 *
	 * Steps:
	 * - Starting resolve connection.
	 * - Resolve callback is called: start address lookup from hostname.
	 * - Hostname lookup finished: choose address, emit resulting peer.
	 *
	 * Errors make the query fail, but do not abort.
	 */
private:
	int hostname_lookup_id{-1}; // used to abort lookup on delete or error
	Peer peer_info;             // info is gathered there at each step

signals:
	void peer_complete (Peer);

public:
	Resolver (uint32_t interface_index, const char * name, const char * regtype, const char * domain,
	          QObject * parent = nullptr)
	    : QObject (parent) {
		peer_info.username = name;
		auto dns_socket = DnsSocket::fromResolve (this, 0 /* flags */, interface_index, name, regtype,
		                                          domain, resolver_callback, this /* context */);
		if (dns_socket != nullptr) {
			connect (dns_socket, &DnsSocket::error, this, &Resolver::deleteLater);
		} else {
			deleteLater ();
		}
	}

	~Resolver () {
		if (hostname_lookup_id != -1)
			QHostInfo::abortHostLookup (hostname_lookup_id);
	}

private:
	static void DNSSD_API resolver_callback (DNSServiceRef, DNSServiceFlags, uint32_t /* interface */,
	                                         DNSServiceErrorType error_code,
	                                         const char * /* fullname */, const char * hostname,
	                                         uint16_t port, uint16_t /* txt len */,
	                                         const unsigned char * /* txt record */, void * context) {
		auto c = static_cast<Resolver *> (context);
		if (error_code == kDNSServiceErr_NoError) {
			c->peer_info.port = qFromBigEndian (port); // To host byte order
			c->peer_info.hostname = hostname;
			c->hostname_lookup_id =
			    QHostInfo::lookupHost (c->peer_info.hostname, c, SLOT (hostname_resolved (QHostInfo)));
		} else {
			qWarning () << "DNSServiceResolve error [callback]:" << error_code;
			c->deleteLater ();
		}
	}

private slots:
	void hostname_resolved (QHostInfo host_info) {
		hostname_lookup_id = -1;
		if (host_info.error () == QHostInfo::NoError) {
			auto addresses = host_info.addresses ();
			if (addresses.isEmpty ())
				qFatal ("QHostInfo returned empty list");
			peer_info.address = addresses.first ();
			emit peer_complete (peer_info);
		} else {
			qWarning () << "QHostInfo failed for" << host_info.hostName () << host_info.errorString ();
		}
		deleteLater ();
	}
};

/*
 * Service browser
 */
class Browser : public QObject {
	Q_OBJECT

	/* Browser object.
	 * Starts browsing at creation, stops at destruction.
	 * Emits added/removed signals to indicate changes to peer list.
	 *
	 * Errors are critical.
	 * TODO expose them and handle them ?
	 */
private:
	QSet<QString> discovered_peers;  // by name
	const QString instance_username; // our username, to discard when browsed

signals:
	void added (Peer);
	void removed (QString);

public:
	Browser (const QString & username, const QString & service_name, QObject * parent = nullptr)
	    : QObject (parent), instance_username (username) {
		DnsSocket::fromBrowse (this, 0 /* flags */, 0 /* interface */, qUtf8Printable (service_name),
		                       nullptr /* default domain */, browser_callback, this /* context */);
	}

private:
	static void DNSSD_API browser_callback (DNSServiceRef, DNSServiceFlags flags,
	                                        uint32_t interface_index, DNSServiceErrorType error_code,
	                                        const char * name, const char * service_name,
	                                        const char * domain, void * context) {
		auto c = static_cast<Browser *> (context);
		if (error_code == kDNSServiceErr_NoError) {
			if (flags & kDNSServiceFlagsAdd) {
				// Peer is added, find its contact info
				auto r = new Resolver (interface_index, name, service_name, domain, c);
				if (r != nullptr) {
					connect (r, &Resolver::peer_complete, c, &Browser::resolved_peer_added);
				}
			} else {
				// Peer is removed
				auto username = QString (name);
				if (username != c->instance_username) {
					auto it = c->discovered_peers.find (username);
					if (it != c->discovered_peers.end ()) {
						c->discovered_peers.erase (it);
						emit c->removed (username);
					} else {
						qWarning () << "Browser: remove: peer does not exists:" << username;
					}
				}
			}
		} else {
			qCritical () << "DNSServiceBrowse error [callback]:" << error_code;
		}
	}

private slots:
	void resolved_peer_added (Peer peer) {
		if (peer.username != instance_username) {
			if (not discovered_peers.contains (peer.username)) {
				discovered_peers.insert (peer.username);
				emit added (peer);
			} else {
				qWarning () << "Browser: add: peer already exists:" << peer.username;
			}
		}
	}
};
}

#endif
