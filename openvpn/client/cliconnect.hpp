//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2012-2015 OpenVPN Technologies, Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

// This file implements the top-level connection logic for an OpenVPN client
// connection.  It is concerned with starting, stopping, pausing, and resuming
// OpenVPN client connections.  It deals with retrying a connection and handles
// the connection timeout.  It also deals with connection exceptions and understands
// the difference between an exception that should halt any further reconnection
// attempts (such as AUTH_FAILED), and other exceptions such as network errors
// that would justify a retry.
//
// Some of the methods in the class (such as stop, pause, and reconnect) are often
// called by another thread that is controlling the connection, therefore
// thread-safe methods are provided where the thread-safe function posts a message
// to the actual connection thread.
//
// In an OpenVPN client connection, the following object stack would be used:
//
// 1. class ClientConnect --
//      The top level object in an OpenVPN client connection.
// 2. class ClientProto::Session --
//      The OpenVPN client protocol object.
// 3. class ProtoContext --
//      The core OpenVPN protocol implementation that is common to both
//      client and server.
// 4. ProtoStackBase<Packet> --
//      The lowest-level class that implements the basic functionality of
//      tunneling a protocol over a reliable or unreliable transport
//      layer, but isn't specific to OpenVPN per-se.

#ifndef OPENVPN_CLIENT_CLICONNECT_H
#define OPENVPN_CLIENT_CLICONNECT_H

#include <memory>

#include <openvpn/common/rc.hpp>
#include <openvpn/error/excode.hpp>
#include <openvpn/time/asiotimer.hpp>
#include <openvpn/client/cliopt.hpp>
#include <openvpn/client/remotelist.hpp>
#include <openvpn/client/clilife.hpp>

namespace openvpn {

  // ClientConnect implements an "always-try-to-reconnect" approach, with remote
  // list rotation.  Only gives up on auth failure or other fatal errors that
  // cannot be remedied by retrying.
  class ClientConnect : ClientProto::NotifyCallback,
			RemoteList::PreResolve::NotifyCallback,
			ClientLifeCycle::NotifyCallback,
			public RC<thread_safe_refcount>
  {
  public:
    typedef boost::intrusive_ptr<ClientConnect> Ptr;
    typedef ClientOptions::Client Client;

    OPENVPN_SIMPLE_EXCEPTION(client_connect_unhandled_exception);

    ClientConnect(boost::asio::io_service& io_service_arg,
		  const ClientOptions::Ptr& client_options_arg)
      : generation(0),
	halt(false),
	paused(false),
	client_finalized(false),
	dont_restart_(false),
	lifecycle_started(false),
	conn_timeout(client_options_arg->conn_timeout()),
	io_service(io_service_arg),
	client_options(client_options_arg),
	server_poll_timer(io_service_arg),
	restart_wait_timer(io_service_arg),
	conn_timer(io_service_arg),
	conn_timer_pending(false)
    {
    }

    void start()
    {
      if (!client && !halt)
	{
	  if (!test_network())
	    throw ErrorCode(Error::NETWORK_UNAVAILABLE, true, "Network Unavailable");

	  RemoteList::Ptr remote_list = client_options->remote_list_precache();
	  RemoteList::PreResolve::Ptr preres(new RemoteList::PreResolve(io_service,
									remote_list,
									client_options->stats_ptr()));
	  if (preres->work_available())
	    {
	      ClientEvent::Base::Ptr ev = new ClientEvent::Resolve();
	      client_options->events().add_event(ev);
	      pre_resolve = preres;
	      pre_resolve->start(this); // asynchronous -- will call back to pre_resolve_done
	    }
	  else
	    new_client();
	}
    }

    void graceful_stop()
    {
      if (!halt && client)
	  client->send_explicit_exit_notify();
      //sleep(5); // simulate slow stop (comment out for production)
      stop();
    }

    void stop()
    {
      if (!halt)
	{
	  halt = true;
	  if (pre_resolve)
	    pre_resolve->cancel();
	  if (client)
	    {
	      client->tun_set_disconnect();
	      client->stop(false);
	    }
	  cancel_timers();
	  asio_work.reset();

	  client_options->finalize(true);

	  if (lifecycle_started)
	    {
	      ClientLifeCycle* lc = client_options->lifecycle();
	      if (lc)
		lc->stop();
	    }

	  ClientEvent::Base::Ptr ev = new ClientEvent::Disconnected();
	  client_options->events().add_event(ev);
	}
    }

    void stop_on_signal(const boost::system::error_code& error, int signal_number)
    {
      stop();
    }

    // like stop() but may be safely called by another thread
    void thread_safe_stop()
    {
      if (!halt)
	io_service.post(asio_dispatch_post(&ClientConnect::graceful_stop, this));
    }

    void pause(const std::string& reason)
    {
      if (!halt && !paused)
	{
	  paused = true;
	  if (client)
	    {
	      client->send_explicit_exit_notify();
	      client->stop(false);
	      interim_finalize();
	    }
	  cancel_timers();
	  asio_work.reset(new boost::asio::io_service::work(io_service));
	  ClientEvent::Base::Ptr ev = new ClientEvent::Pause(reason);
	  client_options->events().add_event(ev);
	  client_options->stats().error(Error::N_PAUSE);
	}
    }

    void resume()
    {
      if (!halt && paused)
	{
	  paused = false;
	  ClientEvent::Base::Ptr ev = new ClientEvent::Resume();
	  client_options->events().add_event(ev);
	  new_client();
	}
    }

    void reconnect(int seconds)
    {
      if (!halt)
	{
	  if (seconds < 0)
	    seconds = 0;
	  OPENVPN_LOG("Client terminated, reconnecting in " << seconds << "...");
	  server_poll_timer.cancel();
	  restart_wait_timer.expires_at(Time::now() + Time::Duration::seconds(seconds));
	  restart_wait_timer.async_wait(asio_dispatch_timer_arg(&ClientConnect::restart_wait_callback, this, generation));
	}
    }

    void thread_safe_pause(const std::string& reason)
    {
      if (!halt)
	io_service.post(asio_dispatch_post_arg(&ClientConnect::pause, this, reason));
    }

    void thread_safe_resume()
    {
      if (!halt)
	io_service.post(asio_dispatch_post(&ClientConnect::resume, this));
    }

    void thread_safe_reconnect(int seconds)
    {
      if (!halt)
	io_service.post(asio_dispatch_post_arg(&ClientConnect::reconnect, this, seconds));
    }

    void dont_restart()
    {
      dont_restart_ = true;
    }

    ~ClientConnect()
    {
      stop();
    }

  private:
    void interim_finalize()
    {
      if (!client_finalized)
	{
	  client_options->finalize(false);
	  client_finalized = true;
	}
    }

    virtual void pre_resolve_done()
    {
      if (!halt)
	new_client();
    }

    void cancel_timers()
    {
      restart_wait_timer.cancel();
      server_poll_timer.cancel();
      conn_timer.cancel();
      conn_timer_pending = false;
    }

    void restart_wait_callback(unsigned int gen, const boost::system::error_code& e)
    {
      if (!e && gen == generation && !halt)
	{
	  if (paused)
	    resume();
	  else
	    {
	      if (client)
		client->send_explicit_exit_notify();
	      new_client();
	    }
	}
    }

    void server_poll_callback(unsigned int gen, const boost::system::error_code& e)
    {
      if (!e && gen == generation && !halt && !client->first_packet_received())
	{
	  OPENVPN_LOG("Server poll timeout, trying next remote entry...");
	  new_client();
	}
    }

    void conn_timer_callback(unsigned int gen, const boost::system::error_code& e)
    {
      if (!e && !halt)
	{
	  client_options->stats().error(Error::CONNECTION_TIMEOUT);
	  if (!paused && client_options->pause_on_connection_timeout())
	    {
	      // go into pause state instead of disconnect
	      pause("");
	    }
	  else
	    {
	      ClientEvent::Base::Ptr ev = new ClientEvent::ConnectionTimeout();
	      client_options->events().add_event(ev);
	      stop();
	    }
	}
    }

    void conn_timer_start()
    {
      if (!conn_timer_pending && conn_timeout > 0)
	{
	  conn_timer.expires_at(Time::now() + Time::Duration::seconds(conn_timeout));
	  conn_timer.async_wait(asio_dispatch_timer_arg(&ClientConnect::conn_timer_callback, this, generation));
	  conn_timer_pending = true;
	}
    }

    bool test_network() const
    {
      ClientLifeCycle* lc = client_options->lifecycle();
      if (lc)
	{
	  if (!lc->network_available())
	    return false;
	}
      return true;
    }

    virtual void client_proto_connected()
    {
      conn_timer.cancel();
      conn_timer_pending = false;

      // Monitor connection lifecycle notifications, such as sleep,
      // wakeup, network-unavailable, and network-available.
      // Not all platforms define a lifecycle object.  Some platforms
      // such as Android and iOS manage lifecycle notifications
      // in the UI, and they call pause(), resume(), reconnect(), etc.
      // as needed using the main ovpncli API.
      if (!lifecycle_started)
	{
	  ClientLifeCycle* lc = client_options->lifecycle(); // lifecycle is defined by platform, and may be NULL
	  if (lc)
	    {
	      lc->start(this);
	      lifecycle_started = true;
	    }
	}
    }

    void queue_restart(const unsigned int delay = 2)
    {
      OPENVPN_LOG("Client terminated, restarting in " << delay << "...");
      server_poll_timer.cancel();
      interim_finalize();
      restart_wait_timer.expires_at(Time::now() + Time::Duration::seconds(delay));
      restart_wait_timer.async_wait(asio_dispatch_timer_arg(&ClientConnect::restart_wait_callback, this, generation));
    }

    virtual void client_proto_terminate()
    {
      if (!halt)
	{
	  if (dont_restart_)
	    {
	      stop();
	    }
	  else
	    {
	      switch (client->fatal())
		{
		case Error::UNDEF: // means that there wasn't a fatal error
		  queue_restart();
		  break;

		// Errors below will cause the client to NOT retry the connection,
		// or otherwise give the error special handling.

		case Error::AUTH_FAILED:
		  {
		    const std::string& reason = client->fatal_reason();
		    if (ChallengeResponse::is_dynamic(reason)) // dynamic challenge/response?
		      {
			ClientEvent::Base::Ptr ev = new ClientEvent::DynamicChallenge(reason);
			client_options->events().add_event(ev);
		      }
		    else
		      {
			ClientEvent::Base::Ptr ev = new ClientEvent::AuthFailed(reason);
			client_options->events().add_event(ev);
			client_options->stats().error(Error::AUTH_FAILED);
		      }
		    stop();
		  }
		  break;
		case Error::TUN_SETUP_FAILED:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::TunSetupFailed(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::TUN_SETUP_FAILED);
		    stop();
		  }
		  break;
		case Error::TUN_IFACE_CREATE:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::TunIfaceCreate(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::TUN_IFACE_CREATE);
		    stop();
		  }
		  break;
		case Error::TUN_IFACE_DISABLED:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::TunIfaceDisabled(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::TUN_IFACE_DISABLED);
		    stop();
		  }
		  break;
		case Error::PROXY_ERROR:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::ProxyError(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::PROXY_ERROR);
		    stop();
		  }
		  break;
		case Error::PROXY_NEED_CREDS:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::ProxyNeedCreds(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::PROXY_NEED_CREDS);
		    stop();
		  }
		  break;
		case Error::CERT_VERIFY_FAIL:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::CertVerifyFail(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::CERT_VERIFY_FAIL);
		    stop();
		  }
		  break;
		case Error::TLS_VERSION_MIN:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::TLSVersionMinFail();
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::TLS_VERSION_MIN);
		    stop();
		  }
		  break;
		case Error::CLIENT_HALT:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::ClientHalt(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::CLIENT_HALT);
		    stop();
		  }
		  break;
		case Error::CLIENT_RESTART:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::ClientRestart(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::CLIENT_RESTART);
		    queue_restart();
		  }
		  break;
		case Error::INACTIVE_TIMEOUT:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::InactiveTimeout();
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::INACTIVE_TIMEOUT);
		    stop();
		  }
		  break;
		case Error::TRANSPORT_ERROR:
		  {
		    ClientEvent::Base::Ptr ev = new ClientEvent::TransportError(client->fatal_reason());
		    client_options->events().add_event(ev);
		    client_options->stats().error(Error::TRANSPORT_ERROR);
		    queue_restart(5); // use a larger timeout to allow preemption from higher levels
		  }
		  break;
		default:
		  throw client_connect_unhandled_exception();
		}
	    }
	}
    }

    void new_client()
    {
      ++generation;
      asio_work.reset();
      if (client)
	{
	  client->stop(false);
	  interim_finalize();
	}
      if (generation > 1)
	{
	  ClientEvent::Base::Ptr ev = new ClientEvent::Reconnecting();
	  client_options->events().add_event(ev);
	  client_options->stats().error(Error::N_RECONNECT);
	  if (!(client && client->reached_connected_state()))
	    client_options->next();
	}
      Client::Config::Ptr cli_config = client_options->client_config(); // client_config in cliopt.hpp
      client.reset(new Client(io_service, *cli_config, this)); // build ClientProto::Session from cliproto.hpp
      client_finalized = false;

      restart_wait_timer.cancel();
      if (client_options->server_poll_timeout_enabled())
	{
	  server_poll_timer.expires_at(Time::now() + client_options->server_poll_timeout());
	  server_poll_timer.async_wait(asio_dispatch_timer_arg(&ClientConnect::server_poll_callback, this, generation));
	}
      conn_timer_start();
      client->start();
    }

    // ClientLifeCycle::NotifyCallback callbacks

    virtual void cln_stop()
    {
      thread_safe_stop();
    }

    virtual void cln_pause(const std::string& reason)
    {
      thread_safe_pause(reason);
    }

    virtual void cln_resume()
    {
      thread_safe_resume();
    }

    virtual void cln_reconnect(int seconds)
    {
      thread_safe_reconnect(seconds);
    }

    unsigned int generation;
    bool halt;
    bool paused;
    bool client_finalized;
    bool dont_restart_;
    bool lifecycle_started;
    int conn_timeout;
    boost::asio::io_service& io_service;
    ClientOptions::Ptr client_options;
    Client::Ptr client;
    AsioTimer server_poll_timer;
    AsioTimer restart_wait_timer;
    AsioTimer conn_timer;
    bool conn_timer_pending;
    std::unique_ptr<boost::asio::io_service::work> asio_work;
    RemoteList::PreResolve::Ptr pre_resolve;
  };

}

#endif
