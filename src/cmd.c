/*
  Copyright (C) 2006, 2007, 2008, 2009  Anthony Catel <a.catel@weelya.com>

  This file is part of APE Server.
  APE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  APE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with APE ; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/* cmd.c */

#include "cmd.h"
#include "hash.h"
#include "json.h"
#include "plugins.h"
#include "config.h"
#include "utils.h"
#include "proxy.h"
#include "raw.h"

void do_register(acetables *g_ape) // register_raw("CMD", Nparam (without IP and time, with sessid), callback_func, NEEDSOMETHING?, g_ape);
{
	register_cmd("CONNECT",		cmd_connect, 	NEED_NOTHING, g_ape);
	register_cmd("PCONNECT",	cmd_pconnect, 	NEED_NOTHING, g_ape);
	register_cmd("SCRIPT",        	cmd_script,		NEED_NOTHING, g_ape);
	
	register_cmd("CHECK", 		cmd_check, 		NEED_SESSID, g_ape);
	register_cmd("SEND", 		cmd_send, 		NEED_SESSID, g_ape);

	register_cmd("QUIT", 		cmd_quit, 		NEED_SESSID, g_ape);
	//register_cmd("SETLEVEL", 	cmd_setlevel, 	NEED_SESSID, g_ape); // Module
	//register_cmd("SETTOPIC", 	cmd_settopic, 	NEED_SESSID, g_ape); // Module
	register_cmd("JOIN", 		cmd_join, 		NEED_SESSID, g_ape);
	register_cmd("LEFT", 		cmd_left, 		NEED_SESSID, g_ape);
	//register_cmd("KICK", 		cmd_kick, 		NEED_SESSID, g_ape); // Module
	//register_cmd("BAN",		cmd_ban,		NEED_SESSID, g_ape); // Module
	//register_cmd("SESSION",        	cmd_session,		NEED_SESSID, g_ape);
	
	//register_cmd("KONG", 		cmd_pong, 		NEED_SESSID, g_ape);
	
	//register_cmd("PROXY_CONNECT", 	cmd_proxy_connect, 	NEED_SESSID, g_ape);
	//register_cmd("PROXY_WRITE", 	cmd_proxy_write, 	NEED_SESSID, g_ape);
}

void register_cmd(const char *cmd, unsigned int (*func)(callbackp *), unsigned int need, acetables *g_ape)
{
	callback *new_cmd, *old_cmd;
	
	new_cmd = (callback *) xmalloc(sizeof(*new_cmd));

	new_cmd->func = func;
	new_cmd->need = need;
	
	/* Unregister old cmd if exists */
	if ((old_cmd = (callback *)hashtbl_seek(g_ape->hCallback, cmd)) != NULL) {
		hashtbl_erase(g_ape->hCallback, cmd);
	}
	
	hashtbl_append(g_ape->hCallback, cmd, (void *)new_cmd);
	
}

void unregister_cmd(const char *cmd, acetables *g_ape)
{
	hashtbl_erase(g_ape->hCallback, cmd);
}

unsigned int checkcmd(clientget *cget, subuser **iuser, acetables *g_ape)
{

	callback *cmdback;
	json_item *ijson, *rjson;
	
	unsigned int flag;
	
	USERS *guser = NULL;
	subuser *sub = NULL;

	ijson = init_json_parser(cget->get);

	/*nTok = explode('&', cget->get, param, 64);
	
	if (nTok < 1) {
		cmd = NULL;
	} else {
		cmd = param[0];

	}*/

	if (ijson == NULL || ijson->child == NULL) {
		SENDH(cget->fdclient, ERR_BAD_JSON, g_ape);
	} else if ((rjson = json_lookup(ijson->child, "cmd")) != NULL &&
			rjson->jval.vu.str.value != NULL && 
			(cmdback = (callback *)hashtbl_seek(g_ape->hCallback, rjson->jval.vu.str.value)) != NULL) {

		int tmpfd = 0;
		callbackp cp;
		switch(cmdback->need) {
			case NEED_SESSID:
				{
					json_item *jsid;
				
					if ((jsid = json_lookup(ijson->child, "sessid")) != NULL && jsid->jval.vu.str.value != NULL) {
						guser = seek_user_id(jsid->jval.vu.str.value, g_ape);
					}
				}
				break;
			case NEED_NOTHING:
				guser = NULL;
				break;
		}
		
		if (cmdback->need != NEED_NOTHING) {
			if (guser == NULL) {
				SENDH(cget->fdclient, ERR_BAD_SESSID, g_ape);
				
				return (CONNECT_SHUTDOWN);
			} else {
				sub = getsubuser(guser, cget->host);
				if (sub != NULL && sub->fd != cget->fdclient && sub->state == ALIVE) {
					if (guser->transport == TRANSPORT_IFRAME) {
						/* iframe is already open on "sub" */
						tmpfd = sub->fd; /* Forward data directly to iframe */
						CLOSE(cget->fdclient, g_ape);
						shutdown(cget->fdclient, 2); /* Immediatly close controller */
					} else {
						/* Only one connection is allowed per user/host */
						CLOSE(sub->fd, g_ape);
						shutdown(sub->fd, 2);
						sub->state = ADIED;
						sub->fd = cget->fdclient;					
					}
				} else if (sub == NULL) {
					sub = addsubuser(cget->fdclient, cget->host, guser, g_ape);
					if (sub != NULL) {
						subuser_restor(sub, g_ape);
					}
				} else if (sub != NULL) {
					sub->fd = cget->fdclient;
				}
				guser->idle = (long int)time(NULL); // update user idle

				sub->idle = guser->idle; // Update subuser idle
			}
		}
		cp.param = json_lookup(ijson->child, "params");
		cp.fdclient = (tmpfd ? tmpfd : cget->fdclient);
		cp.call_user = guser;
		cp.g_ape = g_ape;
		cp.host = cget->host;
		
		flag = cmdback->func(&cp);
		
		if (flag & RETURN_NULL) {
			guser = NULL;
		} else if (flag & RETURN_LOGIN) {
			guser = cp.call_user;
		} else if (flag & RETURN_BAD_PARAMS) {
			guser = NULL;
			SENDH(cget->fdclient, ERR_BAD_PARAM, g_ape);
		}
		
		if (guser != NULL) {

			if (sub == NULL && (sub = getsubuser(guser, cget->host)) == NULL) {
				
				if ((sub = addsubuser(cget->fdclient, cget->host, guser, g_ape)) == NULL) {
					return (CONNECT_SHUTDOWN);
				}
				subuser_restor(sub, g_ape);
				
				if (guser->transport == TRANSPORT_IFRAME) {
					sendbin(sub->fd, HEADER, strlen(HEADER), g_ape);
				}			
			}

			*iuser = (tmpfd ? NULL : sub);
			
			if (flag & RETURN_UPDATE_IP) {
				strncpy(guser->ip, cget->ip_get, 16);
			}
			
			/* If tmpfd is set, we do not have reasons to change this state */
			if (!tmpfd) {
				sub->state = ALIVE;
			}
			return (CONNECT_KEEPALIVE);
			
		}
		return (CONNECT_SHUTDOWN);

	} else { // unregistered CMD
		SENDH(cget->fdclient, ERR_BAD_CMD, g_ape);
	}
	return (CONNECT_SHUTDOWN);
}

unsigned int cmd_connect(callbackp *callbacki)
{
	USERS *nuser;
	RAW *newraw;
	struct json *jstr = NULL;
	
	APE_PARAMS_INIT();
	
	nuser = adduser(callbacki->fdclient, callbacki->host, callbacki->g_ape);
	
	callbacki->call_user = nuser;
	
	if (nuser == NULL) {
		SENDH(callbacki->fdclient, ERR_CONNECT, callbacki->g_ape);
		
		return (RETURN_NOTHING);
	}
	
	if (JINT(transport) == 2) {
		nuser->transport = TRANSPORT_IFRAME;
		nuser->flags |= FLG_PCONNECT;
	} else {
		nuser->transport = TRANSPORT_LONGPOLLING;
	}
	
	subuser_restor(getsubuser(callbacki->call_user, callbacki->host), callbacki->g_ape);
	
	set_json("sessid", nuser->sessid, &jstr);
	
	newraw = forge_raw(RAW_LOGIN, jstr);
	newraw->priority = 1;
	
	post_raw(newraw, nuser, callbacki->g_ape);
	
	
	return (RETURN_LOGIN | RETURN_UPDATE_IP);

}
/* Deprecated */
unsigned int cmd_pconnect(callbackp *callbacki)
{
	USERS *nuser;

	nuser = adduser(callbacki->fdclient, callbacki->host, callbacki->g_ape);
	if (nuser == NULL) {
		SENDH(callbacki->fdclient, ERR_CONNECT, callbacki->g_ape);
		
		return (RETURN_NOTHING);
	}
	nuser->flags |= FLG_PCONNECT;
	
	return (RETURN_LOGIN | RETURN_UPDATE_IP);

}

unsigned int cmd_script(callbackp *callbacki)
{
	/*char *domain = CONFIG_VAL(Server, domain, callbacki->g_ape->srv);
	if (domain == NULL) {
		send_error(callbacki->call_user, "NO_DOMAIN", "201", callbacki->g_ape);
	} else {
		int i;
		sendf(callbacki->fdclient, callbacki->g_ape, "%s<html>\n<head>\n\t<script>\n\t\tdocument.domain=\"%s\"\n\t</script>\n", HEADER, domain);
		for (i = 1; i <= callbacki->nParam; i++) {
			sendf(callbacki->fdclient, callbacki->g_ape, "\t<script type=\"text/javascript\" src=\"%s\"></script>\n", callbacki->param[i]);
		}
		sendbin(callbacki->fdclient, "</head>\n<body>\n</body>\n</html>", 30, callbacki->g_ape);
	}*/
	return (RETURN_NOTHING);
}

unsigned int cmd_join(callbackp *callbacki)
{
	CHANNEL *jchan;
	RAW *newraw;
	json *jlist;
	BANNED *blist;
	
	APE_PARAMS_INIT();
	
	char *chan_name = JSTR(channel);
	
	if (chan_name != NULL) {
	
		if ((jchan = getchan(chan_name, callbacki->g_ape)) == NULL) {
			jchan = mkchan(chan_name, "Default%20Topic", callbacki->g_ape);
		
			if (jchan == NULL) {
			
				send_error(callbacki->call_user, "CANT_JOIN_CHANNEL", "202", callbacki->g_ape);
			
			} else {
		
				join(callbacki->call_user, jchan, callbacki->g_ape);
			}
	
		} else if (isonchannel(callbacki->call_user, jchan)) {
		
			send_error(callbacki->call_user, "ALREADY_ON_CHANNEL", "100", callbacki->g_ape);

		} else {
			blist = getban(jchan, callbacki->call_user->ip);
			if (blist != NULL) {
				jlist = NULL;
			
				set_json("reason", blist->reason, &jlist);
				set_json("error", "YOU_ARE_BANNED", &jlist);
				/*
					TODO: Add Until
				*/
				newraw = forge_raw(RAW_ERR, jlist);
			
				post_raw(newraw, callbacki->call_user, callbacki->g_ape);
			} else {
				join(callbacki->call_user, jchan, callbacki->g_ape);
			}
		}
		return (RETURN_NOTHING);
	}
	return (RETURN_BAD_PARAMS);
}

unsigned int cmd_check(callbackp *callbacki)
{
	return (RETURN_NOTHING);
}

unsigned int cmd_send(callbackp *callbacki)
{
	json *jlist = NULL;
	char *msg, *pipe;
	
	APE_PARAMS_INIT();
	
	if ((msg = JSTR(msg)) != NULL && (pipe = JSTR(pipe)) != NULL) {
	
		set_json("msg", msg, &jlist);

		post_to_pipe(jlist, RAW_DATA, pipe, getsubuser(callbacki->call_user, callbacki->host), NULL, callbacki->g_ape);
		
		return (RETURN_NOTHING);
	}
	
	return (RETURN_BAD_PARAMS);
	
}
unsigned int cmd_quit(callbackp *callbacki)
{
	QUIT(callbacki->fdclient, callbacki->g_ape);
	deluser(callbacki->call_user, callbacki->g_ape); // After that callbacki->call_user is free'd
	
	return (RETURN_NULL);
}

#if 0
unsigned int cmd_setlevel(callbackp *callbacki)
{
	USERS *recver;
	
	if ((recver = seek_user(callbacki->param[3], callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_USER", "102", callbacki->g_ape);
	} else {
		setlevel(callbacki->call_user, recver, getchan(callbacki->param[2], callbacki->g_ape), atoi(callbacki->param[4]), callbacki->g_ape);
	}
	return (RETURN_NOTHING);
}
#endif

unsigned int cmd_left(callbackp *callbacki)
{
	CHANNEL *chan;
	char *chan_name;
	
	APE_PARAMS_INIT();
	
	if ((chan_name = JSTR(channel)) != NULL) {
	
		if ((chan = getchan(chan_name, callbacki->g_ape)) == NULL) {
			send_error(callbacki->call_user, "UNKNOWN_CHANNEL", "103", callbacki->g_ape);
		
		} else if (!isonchannel(callbacki->call_user, chan)) {
			send_error(callbacki->call_user, "NOT_IN_CHANNEL", "104", callbacki->g_ape);
	
		} else {
	
			left(callbacki->call_user, chan, callbacki->g_ape);
		}
		
		return (RETURN_NOTHING);
	}
	
	return (RETURN_BAD_PARAMS);
}

#if 0
unsigned int cmd_settopic(callbackp *callbacki)
{
	settopic(callbacki->call_user, getchan(callbacki->param[2], callbacki->g_ape), callbacki->param[3], callbacki->g_ape);
	
	return (RETURN_NOTHING);
}

unsigned int cmd_kick(callbackp *callbacki)
{
	CHANNEL *chan;
	RAW *newraw;
	json *jlist;
	
	USERS *victim;

	if ((chan = getchan(callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_CHANNEL", "103", callbacki->g_ape);
		
	} else if (!isonchannel(callbacki->call_user, chan)) {
		send_error(callbacki->call_user, "NOT_IN_CHANNEL", "104", callbacki->g_ape);
		
	} else if (getuchan(callbacki->call_user, chan)->level < 3) {
		send_error(callbacki->call_user, "CANT_KICK", "105", callbacki->g_ape);
		
	} else {
		victim = seek_user(callbacki->param[3], chan->pipe->pubid, callbacki->g_ape);
		
		if (victim == NULL) {

			send_error(callbacki->call_user, "UNKNOWN_USER", "102", callbacki->g_ape);
		} else if (victim->flags & FLG_NOKICK) {
			
			send_error(callbacki->call_user, "USER_PROTECTED", "106", callbacki->g_ape);
			// haha ;-)
			
			jlist = NULL;
			set_json("kicker", NULL, &jlist);
			json_attach(jlist, get_json_object_user(callbacki->call_user), JSON_OBJECT);
			
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
						
			newraw = forge_raw("TRY_KICK", jlist);
			
			post_raw(newraw, victim, callbacki->g_ape);
			
		} else {
			jlist = NULL;
			
			set_json("kicker", NULL, &jlist);
			json_attach(jlist, get_json_object_user(callbacki->call_user), JSON_OBJECT);
			
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
						
			newraw = forge_raw(RAW_KICK, jlist);
			
			post_raw(newraw, victim, callbacki->g_ape);
			
			left(victim, chan, callbacki->g_ape); // chan may be removed
			
		}
		
	}
	
	return (RETURN_NOTHING);
	
}

unsigned int cmd_ban(callbackp *callbacki)
{
	CHANNEL *chan;
	RAW *newraw;
	json *jlist;
	
	USERS *victim;

	if ((chan = getchan(callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_CHANNEL", "103", callbacki->g_ape);
		
	
	} else if (!isonchannel(callbacki->call_user, chan)) {

		send_error(callbacki->call_user, "NOT_IN_CHANNEL", "104", callbacki->g_ape);
	
	} else if (getuchan(callbacki->call_user, chan)->level < 3) {

		
		send_error(callbacki->call_user, "CANT_BAN", "107", callbacki->g_ape);
		
	} else {
		victim = seek_user(callbacki->param[3], chan->name, callbacki->g_ape);
		
		if (victim == NULL) {

			send_error(callbacki->call_user, "UNKNOWN_USER", "102", callbacki->g_ape);
			
		} else if (victim->flags & FLG_NOKICK) {
			send_error(callbacki->call_user, "USER_PROTECTED", "106", callbacki->g_ape);
			
			// Bad boy :-)
			jlist = NULL;
			set_json("banner", NULL, &jlist);
			json_attach(jlist, get_json_object_user(callbacki->call_user), JSON_OBJECT);
			
			set_json("channel", NULL, &jlist);
			json_attach(jlist, get_json_object_channel(chan), JSON_OBJECT);
						
			newraw = forge_raw("TRY_BAN", jlist);
			
			post_raw(newraw, victim, callbacki->g_ape);
			
		} else if (strlen(callbacki->param[4]) > 255 || atoi(callbacki->param[5]) > 44640) { // 31 days max

			send_error(callbacki->call_user, "REASON_OR_TIME_TOO_LONG", "107", callbacki->g_ape);
		} else {
			ban(chan, callbacki->call_user, victim->ip, callbacki->param[4], atoi(callbacki->param[5]), callbacki->g_ape);
		}
		
	}
	
	return (RETURN_NOTHING);
}



unsigned int cmd_session(callbackp *callbacki)
{
	if (strcmp(callbacki->param[2], "set") == 0 && (callbacki->nParam == 4 || callbacki->nParam == 5)) {
		if (callbacki->nParam == 5) {
			subuser *tmpSub = getsubuser(callbacki->call_user, callbacki->host);
		
			if (tmpSub != NULL) {
				tmpSub->need_update = 0;
			}
		}
		if (set_session(callbacki->call_user, callbacki->param[3], callbacki->param[4], (callbacki->nParam == 4 ? 0 : 1), callbacki->g_ape) == NULL) {
			send_error(callbacki->call_user, "SESSION_ERROR", "203", callbacki->g_ape);
		}
	} else if (strcmp(callbacki->param[2], "get") == 0 && callbacki->nParam >= 3) {
		int i;
		json *jlist = NULL, *jobj = NULL;
		RAW *newraw;
		
		set_json("sessions", NULL, &jlist);
		
		for (i = 3; i <= callbacki->nParam; i++) {
			if (strlen(callbacki->param[i]) > 32) {
				continue;
			}
			session *sTmp = get_session(callbacki->call_user, callbacki->param[i]);

			set_json(callbacki->param[i], (sTmp != NULL ? sTmp->val : NULL), &jobj);

		}
		json_attach(jlist, jobj, JSON_OBJECT);
		newraw = forge_raw("SESSIONS", jlist);
		newraw->priority = 1;
		/* Only sending to current subuser */
		post_raw_sub(newraw, getsubuser(callbacki->call_user, callbacki->host), callbacki->g_ape);

	} else {
		send_error(callbacki->call_user, "SESSION_ERROR_PARAMS", "108", callbacki->g_ape);
	}
	return (RETURN_NOTHING);
}

/* This is usefull to ask all subuser to update their sessions */
unsigned int cmd_pong(callbackp *callbacki)
{
	if (strcmp(callbacki->param[2], callbacki->call_user->lastping) == 0) {
		RAW *newraw;
				
		callbacki->call_user->lastping[0] = '\0';

		json *jlist = NULL;
	
		set_json("value", callbacki->param[2], &jlist);
	
		newraw = forge_raw("UPDATE", jlist);
	
		post_raw_sub(newraw, getsubuser(callbacki->call_user, callbacki->host), callbacki->g_ape);
	}
	return (RETURN_NOTHING);
}


unsigned int cmd_proxy_connect(callbackp *callbacki)
{
	ape_proxy *proxy;
	RAW *newraw;
	json *jlist = NULL;
	
	proxy = proxy_init_by_host_port(callbacki->param[2], callbacki->param[3], callbacki->g_ape);
	
	if (proxy == NULL) {
		send_error(callbacki->call_user, "PROXY_INIT_ERROR", "204", callbacki->g_ape);
	} else {
		proxy_attach(proxy, callbacki->call_user->pipe->pubid, 1, callbacki->g_ape);
		
		set_json("pipe", NULL, &jlist);
		json_attach(jlist, get_json_object_proxy(proxy), JSON_OBJECT);
	
		newraw = forge_raw(RAW_PROXY, jlist);
		post_raw(newraw, callbacki->call_user, callbacki->g_ape);		
	}
	
	return (RETURN_NOTHING);
}

unsigned int cmd_proxy_write(callbackp *callbacki)
{
	ape_proxy *proxy;

	if ((proxy = proxy_are_linked(callbacki->call_user->pipe->pubid, callbacki->param[2], callbacki->g_ape)) == NULL) {
		send_error(callbacki->call_user, "UNKNOWN_PIPE", "109", callbacki->g_ape);
	} else if (proxy->state != PROXY_CONNECTED) {
		send_error(callbacki->call_user, "PROXY_NOT_CONNETED", "205", callbacki->g_ape);
	} else {
		proxy_write(proxy, callbacki->param[3], callbacki->g_ape);
	}
	
	return (RETURN_NOTHING);
}
#endif
