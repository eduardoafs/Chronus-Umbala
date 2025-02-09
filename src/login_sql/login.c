// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include <sys/types.h>

#ifdef LCCWIN32
#include <winsock.h>
#pragma lib <libmysql.lib>
#else
#ifdef __WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <time.h>
void Gettimeofday(struct timeval *timenow)
{
	time_t t;
	t = clock();
	timenow->tv_usec = (long)t;
	timenow->tv_sec = (long)(t / CLK_TCK);
	return;
}
#define gettimeofday(timenow, dummy) Gettimeofday(timenow)
#define in_addr_t unsigned long
#pragma comment(lib,"libmysql.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h> // for stat/lstat/fstat
#include <signal.h>
#include <fcntl.h>
#include <string.h>

//add include for DBMS(mysql)
#include <mysql.h>

#include "../common/core.h"
#include "../common/socket.h"
#include "../common/malloc.h"
#include "../common/db.h"
#include "../common/timer.h"
#include "../common/strlib.h"
#include "../common/mmo.h"
#include "../common/showmsg.h"
#include "../common/version.h"
#include "login.h"

#ifdef PASSWORDENC
#include "md5calc.h"
#endif

#define J_MAX_MALLOC_SIZE 65535

//-----------------------------------------------------
// global variable
//-----------------------------------------------------
int account_id_count = START_ACCOUNT_NUM;
int server_num;
int new_account_flag = 0; //Set from config too XD [Sirius]
int bind_ip_set_ = 0;
char bind_ip_str[128];
int login_port = 6900;
char lan_char_ip[128]; // Lan char ip added by kashy
int subnetmaski[4]; // Subnetmask added by kashy

struct mmo_char_server server[MAX_SERVERS];
int server_fd[MAX_SERVERS];

int login_fd;

//Account flood protection [Kevin]
unsigned int new_reg_tick=0;
int allowed_regs=1;
int num_regs=0;
int time_allowed=10; //Init this to 10 secs, not 10K secs [Skotlex]

char date_format[32] = "%Y-%m-%d %H:%M:%S";
unsigned int auth_num = 0, auth_max = 0;

int min_level_to_connect = 0; // minimum level of player/GM (0: player, 1-99: gm) to connect on the server
int check_ip_flag = 1; // It's to check IP of a player between login-server and char-server (part of anti-hacking system)
int check_client_version = 0; //Client version check ON/OFF .. (sirius)
int client_version_to_connect = 20; //Client version needed to connect ..(sirius)
static int online_check=1; //When set to 1, login server rejects incoming players that are already registered as online. [Skotlex]

MYSQL mysql_handle;

int ipban = 1;
int dynamic_account_ban = 1;
int dynamic_account_ban_class = 0;
int dynamic_pass_failure_ban = 1;
int dynamic_pass_failure_ban_time = 5;
int dynamic_pass_failure_ban_how_many = 3;
int dynamic_pass_failure_ban_how_long = 60;

int login_server_port = 3306;
char login_server_ip[32] = "127.0.0.1";
char login_server_id[32] = "ragnarok";
char login_server_pw[32] = "ragnarok";
char login_server_db[32] = "ragnarok";
char default_codepage[32] = ""; //Feature by irmin.
int use_md5_passwds = 0;
char login_db[256] = "login";
int log_login=1; //Whether to log the logins or not. [Skotlex]
char loginlog_db[256] = "loginlog";

// added to help out custom login tables, without having to recompile
// source so options are kept in the login_athena.conf or the inter_athena.conf
char login_db_account_id[256] = "account_id";
char login_db_userid[256] = "userid";
char login_db_user_pass[256] = "user_pass";
char login_db_level[256] = "level";

char reg_db[256] = "global_reg_value";

int lowest_gm_level;
struct gm_account *gm_account_db;
int GM_num;
char tmpsql[65535], tmp_sql[65535];

int console = 0;

int case_sensitive = 1;

//-----------------------------------------------------

#define AUTH_FIFO_SIZE 256
struct {
	int account_id,login_id1,login_id2;
	int ip,sex,delflag;
} auth_fifo[AUTH_FIFO_SIZE];

int auth_fifo_pos = 0;

struct online_login_data {
	int account_id;
	short char_server;
	short waiting_disconnect;
};

//-----------------------------------------------------

static char md5key[20], md5keylen = 16;

struct dbt *online_db;

static int online_db_final(void *key,void *data,va_list ap)
{
	int *p = (int *) data;
	if (p) aFree(p);
	return 0;
}

//-----------------------------------------------------
// Online User Database [Wizputer]
//-----------------------------------------------------

void add_online_user(int char_server, int account_id) {
	struct online_login_data *p;
	if (!online_check)
		return;
	p = numdb_search(online_db, account_id);
	if (p == NULL) {
		p = aCalloc(1, sizeof(struct online_login_data));
		p->account_id = account_id;
		p->char_server = char_server;
		numdb_insert(online_db, account_id, p);
	} else {
		p->char_server = char_server;
		p->waiting_disconnect = 0;
	}
}

int is_user_online(int account_id) {
	struct online_login_data *p;

	p = numdb_search(online_db, account_id);

	return (p != NULL);
}

void remove_online_user(int account_id) {
	struct online_login_data *p;
	if(!online_check)
		return;
	if (account_id == 99) {	// reset all to offline
		numdb_final(online_db, online_db_final);	// purge db
		online_db = numdb_init();	// reinitialise
		return;
	}
	p = numdb_erase(online_db,account_id);
	if (p) aFree(p);
}

int waiting_disconnect_timer(int tid, unsigned int tick, int id, int data)
{
	struct online_login_data *p;
	if ((p= numdb_search(online_db, id)) != NULL && p->waiting_disconnect)
		remove_online_user(p->account_id);
	return 0;
}

//-----------------------------------------------------
// Read GM accounts
//-----------------------------------------------------
void read_gm_account(void) {
	MYSQL_RES* sql_res ;
	MYSQL_ROW sql_row;

	if (gm_account_db != NULL)
		aFree(gm_account_db);
	GM_num = 0;

	sprintf(tmp_sql, "SELECT `%s`,`%s` FROM `%s` WHERE `%s`>='%d'",login_db_account_id,login_db_level,login_db,login_db_level,lowest_gm_level);
	if (mysql_query(&mysql_handle, tmp_sql)) {
		ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
		ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
	}
	sql_res = mysql_store_result(&mysql_handle);
	if (sql_res) {
		gm_account_db = (struct gm_account*)aCalloc((size_t)mysql_num_rows(sql_res), sizeof(struct gm_account));
		while ((sql_row = mysql_fetch_row(sql_res))) {
			gm_account_db[GM_num].account_id = atoi(sql_row[0]);
			gm_account_db[GM_num].level = atoi(sql_row[1]);
			GM_num++;
		}
	}

	mysql_free_result(sql_res);
}

int charif_sendallwos(int sfd, unsigned char *buf, unsigned int len);

//-----------------------------------------------------
// Send GM accounts to all char-server
//-----------------------------------------------------
void send_GM_accounts(int fd) {
	int i;
	unsigned char buf[32767];
	int len;

	len = 4;
	WBUFW(buf,0) = 0x2732;
	for(i = 0; i < GM_num; i++)
		// send only existing accounts. We can not create a GM account when server is online.
		if (gm_account_db[i].level > 0) {
			WBUFL(buf,len) = gm_account_db[i].account_id;
			WBUFB(buf,len+4) = (unsigned char)gm_account_db[i].level;
			len += 5;
		}
	WBUFW(buf,2) = len;
	if (fd == -1)
		charif_sendallwos(-1, buf, len);
	else
	{
		memcpy(WFIFOP(fd,0), buf, len);
		WFIFOSET(fd,len);
	}
	return;
}

//-----------------------------------------------------
// check user level
//-----------------------------------------------------
/*
int isGM(int account_id) {
	int level;

	MYSQL_RES* 	sql_res;
	MYSQL_ROW	sql_row;
	level = 0;
	sprintf(tmpsql,"SELECT `%s` FROM `%s` WHERE `%s`='%d'", login_db_level, login_db, login_db_account_id, account_id);
	if (mysql_query(&mysql_handle, tmpsql)) {
		ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
		ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
	}
	sql_res = mysql_store_result(&mysql_handle);
	if (sql_res) {
		sql_row = mysql_fetch_row(sql_res);
		level = atoi(sql_row[0]);
		if (level > 99)
			level = 99;
	}

	if (level == 0) {
		return 0;
		//not GM
	}

	mysql_free_result(sql_res);

	return level;
}
*/

//---------------------------------------------------
// E-mail check: return 0 (not correct) or 1 (valid).
//---------------------------------------------------
int e_mail_check(char *email) {
	char ch;
	char* last_arobas;

	// athena limits
	if (strlen(email) < 3 || strlen(email) > 39)
		return 0;

	// part of RFC limits (official reference of e-mail description)
	if (strchr(email, '@') == NULL || email[strlen(email)-1] == '@')
		return 0;

	if (email[strlen(email)-1] == '.')
		return 0;

	last_arobas = strrchr(email, '@');

	if (strstr(last_arobas, "@.") != NULL ||
	    strstr(last_arobas, "..") != NULL)
		return 0;

	for(ch = 1; ch < 32; ch++) {
		if (strchr(last_arobas, ch) != NULL) {
			return 0;
			break;
		}
	}

	if (strchr(last_arobas, ' ') != NULL ||
	    strchr(last_arobas, ';') != NULL)
		return 0;

	// all correct
	return 1;
}

//-----------------------------------------------------
// Read Account database - mysql db
//-----------------------------------------------------
int mmo_auth_sqldb_init(void) {

	ShowStatus("Login server init....\n");

	// memory initialize
	ShowStatus("memory initialize....\n");

	mysql_init(&mysql_handle);

	// DB connection start
	ShowStatus("Connect Login Database Server....\n");
	if (!mysql_real_connect(&mysql_handle, login_server_ip, login_server_id, login_server_pw,
	    login_server_db, login_server_port, (char *)NULL, 0)) {
		// pointer check
		ShowFatalError("%s\n", mysql_error(&mysql_handle));
		exit(1);
	} else {
		ShowStatus("Connect success!\n");
	}
	if( strlen(default_codepage) > 0 ) {
		sprintf( tmpsql, "SET NAMES %s", default_codepage );
		if (mysql_query(&mysql_handle, tmpsql)) {
			ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
			ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmpsql);
		}
	}

	if (log_login)
	{
		sprintf(tmpsql, "INSERT DELAYED INTO `%s`(`time`,`ip`,`user`,`rcode`,`log`) VALUES (NOW(), '', 'lserver', '100','login server started')", loginlog_db);

		//query
		if (mysql_query(&mysql_handle, tmpsql)) {
			ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
			ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
		}
	}
	if (new_account_flag)
	{	//Check if the next new account will need to have it's ID set (to avoid bad DBs which would otherwise insert
		//new accounts with account_ids of less than 2M [Skotlex]
		sprintf(tmp_sql, "SELECT max(`%s`) from `%s`", login_db_account_id, login_db);
		if(mysql_query(&mysql_handle, tmp_sql)){
			ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
			ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
		} else {
			MYSQL_RES* 	sql_res;
			MYSQL_ROW	sql_row;

			sql_res = mysql_store_result(&mysql_handle) ;
			if (sql_res)
			{
				if (mysql_num_rows(sql_res) > 0 &&
					(sql_row = mysql_fetch_row(sql_res)) != NULL &&
					sql_row[0] != NULL && atoi(sql_row[0]) >= account_id_count)
				//Ok, chars already exist, no need to use this.
					account_id_count = 0;
				mysql_free_result(sql_res);
			}
		}
	}
	return 0;
}

//-----------------------------------------------------
// DB server connect check
//-----------------------------------------------------
void mmo_auth_sqldb_sync(void) {
	// db connect check? or close?
	// ping pong DB server -if losted? then connect try. else crash.
}

//-----------------------------------------------------
// close DB
//-----------------------------------------------------
void mmo_db_close(void) {
	int i, fd;

	//set log.
	if (log_login)
	{
		sprintf(tmpsql,"INSERT DELAYED INTO `%s`(`time`,`ip`,`user`,`rcode`,`log`) VALUES (NOW(), '', 'lserver','100', 'login server shutdown')", loginlog_db);

		//query
		if (mysql_query(&mysql_handle, tmpsql)) {
			ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
			ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
		}
	}
/*
	//delete all server status
	sprintf(tmpsql,"DELETE FROM `sstatus`");
	//query
	if (mysql_query(&mysql_handle, tmpsql)) {
		ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
		ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
	}
	mysql_close(&mysql_handle);
	ShowStatus("close DB connect....\n");
*/

	for (i = 0; i < MAX_SERVERS; i++) {
		if ((fd = server_fd[i]) >= 0)
		{	//Clean only data related to servers we are connected to. [Skotlex]
			sprintf(tmpsql,"DELETE FROM `sstatus` WHERE `index` = '%d'", i);
			if (mysql_query(&mysql_handle, tmpsql))
			{
				ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
				ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
			}
			delete_session(fd);
		}
	}
	mysql_close(&mysql_handle);
	ShowStatus("close DB connect....\n");
	delete_session(login_fd);
}

//-----------------------------------------------------
// Make new account
//-----------------------------------------------------
int mmo_auth_new(struct mmo_account* account, char sex)
{
	MYSQL_RES* 	sql_res;
	unsigned int tick = gettick();
	char user_password[256];
	//Account Registration Flood Protection by [Kevin]
	if(tick <= new_reg_tick && num_regs >= allowed_regs) {
		ShowNotice("Account registration denied (registration limit exceeded)\n");
		return 3;
	}

	//Check for preexisting account
	sprintf(tmp_sql, "SELECT `%s` FROM `%s` WHERE `userid` = '%s'", login_db_userid, login_db, account->userid);
	if(mysql_query(&mysql_handle, tmp_sql)){
		ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
		ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
		return 1; //Return Incorrect user/pass?
	}

	sql_res = mysql_store_result(&mysql_handle);
	if(mysql_num_rows(sql_res) > 0){
		mysql_free_result(sql_res);
		return 1; //Already exists, return incorrect user/pass.
	}
	mysql_free_result(sql_res); //Only needed for the already-exists check...

	mysql_real_escape_string(&mysql_handle, account->userid, account->userid, strlen(account->userid));
	mysql_real_escape_string(&mysql_handle, account->passwd, account->passwd, strlen(account->passwd));

	if (sex == 'f') sex = 'F';
	else if (sex == 'm') sex = 'M';
	if (use_md5_passwds)
		MD5_String(account->passwd,user_password);
	else
		jstrescapecpy(user_password, account->passwd);

	ShowInfo("New account: user: %s with passwd: %s sex: %c\n", account->userid, user_password, sex);

	if (account_id_count) //Force new Account ID
		sprintf(tmp_sql, "INSERT INTO `%s` (`%s`, `%s`, `%s`, `sex`, `email`) VALUES ('%d', '%s', '%s', '%c', '%s')", login_db, login_db_account_id, login_db_userid, login_db_user_pass, account_id_count, account->userid, user_password, sex, "a@a.com");
	else
		sprintf(tmp_sql, "INSERT INTO `%s` (`%s`, `%s`, `sex`, `email`) VALUES ('%s', '%s', '%c', '%s')", login_db, login_db_userid, login_db_user_pass, account->userid, user_password, sex, "a@a.com");

	if(mysql_query(&mysql_handle, tmp_sql)){
		//Failed to insert new acc :/
		ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
		ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
		return 1;
	}

	if (account_id_count) //Clear it or all new accounts will try to use the same id :P
		account_id_count = 0;

	if(tick > new_reg_tick)
	{	//Update the registration check.
		num_regs=0;
		new_reg_tick=gettick()+time_allowed*1000;
	}
	num_regs++;

	return 0;
}

#ifdef LCCWIN32
extern void gettimeofday(struct timeval *t, struct timezone *dummy);
#endif

// Send to char
int charif_sendallwos(int sfd, unsigned char *buf, unsigned int len) {
	int i, c;
	int fd;

	c = 0;
	for(i = 0; i < MAX_SERVERS; i++) {
		if ((fd = server_fd[i]) > 0 && fd != sfd) {
			if (WFIFOSPACE(fd) < len) //Increase buffer size.
				  realloc_writefifo(fd, len);
			memcpy(WFIFOP(fd,0), buf, len);
			WFIFOSET(fd,len);
			c++;
		}
	}

	return c;
}

//-----------------------------------------------------
// Auth
//-----------------------------------------------------
int mmo_auth( struct mmo_account* account , int fd){
	time_t ban_until_time, raw_time;
	char tmpstr[256];
	char t_uid[256], t_pass[256];
	char user_password[256];

	//added for account creation _M _F
	int len;

	MYSQL_RES* 	sql_res;
	MYSQL_ROW	sql_row;
	//int sql_fields, sql_cnt;
	char md5str[64], md5bin[32];

	char ip[16];

	unsigned char *sin_addr = (unsigned char *)&session[fd]->client_addr.sin_addr;


	sprintf(ip, "%d.%d.%d.%d", sin_addr[0], sin_addr[1], sin_addr[2], sin_addr[3]);
	ShowInfo("auth start for %s...\n", ip);

	//accountreg with _M/_F .. [Sirius]
	len = strlen(account->userid) -2;

	if (account->passwdenc == 0 && account->userid[len] == '_' &&
		(account->userid[len+1] == 'F' || account->userid[len+1] == 'M' ||
		account->userid[len+1] == 'f' || account->userid[len+1] == 'm') &&
		new_account_flag == 1 &&
		len >= 4 && strlen(account->passwd) >= 4)
	{
		int result;
		account->userid[len] = '\0'; //Terminating the name.
		if ((result = mmo_auth_new(account, account->userid[len+1])))
			return result; //Failed to make account. [Skotlex].
	}

 	// auth start : time seed
	// Platform/Compiler dependant clock() for time check is removed. [Lance]
	// clock() is originally used to track processing ticks on program execution.
	time(&raw_time);
	strftime(tmpstr, 24, "%Y-%m-%d %H:%M:%S",localtime(&raw_time));

	jstrescapecpy(t_uid,account->userid);
	jstrescapecpy(t_pass, account->passwd);


	// make query
	sprintf(tmpsql, "SELECT `%s`,`%s`,`%s`,`lastlogin`,`logincount`,`sex`,`connect_until`,`last_ip`,`ban_until`,`state`,`%s`"
	                " FROM `%s` WHERE %s `%s`='%s'", login_db_account_id, login_db_userid, login_db_user_pass, login_db_level, login_db, case_sensitive ? "BINARY" : "", login_db_userid, t_uid);
	//login {0-account_id/1-userid/2-user_pass/3-lastlogin/4-logincount/5-sex/6-connect_untl/7-last_ip/8-ban_until/9-state/10-level}

	// query
	if (mysql_query(&mysql_handle, tmpsql)) {
		ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
		ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
	}
	sql_res = mysql_store_result(&mysql_handle) ;
	if (sql_res) {
		sql_row = mysql_fetch_row(sql_res);	//row fetching
		if (!sql_row) {
			//there's no id.
			ShowNotice("auth failed: no such account %s %s %s\n", tmpstr, account->userid, account->passwd);
			mysql_free_result(sql_res);
			return 0;
		}
	} else {
		ShowError("mmo_auth DB result error ! \n");
		return 0;
	}

	//Client Version check[Sirius]
        if(check_client_version == 1 && account->version != 0){
        	if(account->version != client_version_to_connect){
			mysql_free_result(sql_res);
			return 5;
		}
	}

	// Documented by CLOWNISIUS || LLRO || Gunstar lead this one with me
	// IF changed to diferent returns~ you get diferent responses from your msgstringtable.txt
	//Ireturn 2  == line 9
	//Ireturn 5  == line 311
	//Ireturn 6  == line 450
	//Ireturn 7  == line 440
	//Ireturn 8  == line 682
	//Ireturn 9  == line 704
	//Ireturn 10 == line 705
	//Ireturn 11 == line 706
	//Ireturn 12 == line 707
	//Ireturn 13 == line 708
	//Ireturn 14 == line 709
	//Ireturn 15 == line 710
	//Ireturn -1 == line 010
	// Check status
	{
		int encpasswdok = 0;

		if (atoi(sql_row[9]) == -3) {
			//id is banned
			mysql_free_result(sql_res);
			return -3;
		} else if (atoi(sql_row[9]) == -2) { //dynamic ban
			//id is banned
			mysql_free_result(sql_res);
			//add IP list.
			return -2;
		}

		if (use_md5_passwds) {
			MD5_String(account->passwd,user_password);
		} else {
			jstrescapecpy(user_password, account->passwd);
		}
		ShowInfo("account id ok encval:%d\n",account->passwdenc);
#ifdef PASSWORDENC
		if (account->passwdenc > 0) {
			int j = account->passwdenc;
			ShowInfo("start md5calc..\n");
			if (j > 2)
				j = 1;
			do {
				if (j == 1) {
					sprintf(md5str, "%s%s", md5key,sql_row[2]);
				} else if (j == 2) {
					sprintf(md5str, "%s%s", sql_row[2], md5key);
				} else
					md5str[0] = 0;
				ShowDebug("j:%d mdstr:%s\n", j, md5str);
				MD5_String2binary(md5str, md5bin);
				encpasswdok = (memcmp(user_password, md5bin, 16) == 0);
			} while (j < 2 && !encpasswdok && (j++) != account->passwdenc);
			//printf("key[%s] md5 [%s] ", md5key, md5);
			ShowInfo("client [%s] accountpass [%s]\n", user_password, sql_row[2]);
			ShowInfo("end md5calc..\n");
		}
#endif
		if ((strcmp(user_password, sql_row[2]) && !encpasswdok)) {
			if (account->passwdenc == 0) {
				ShowNotice("auth failed pass error %s %s %s" RETCODE, tmpstr, account->userid, user_password);
#ifdef PASSWORDENC
			} else {
				char logbuf[1024], *p = logbuf;
				int j;
				p += sprintf(p, "auth failed pass error %s %s recv-md5[", tmpstr, account->userid);
				for(j = 0; j < 16; j++)
					p += sprintf(p, "%02x", ((unsigned char *)user_password)[j]);
				p += sprintf(p, "] calc-md5[");
				for(j = 0; j < 16; j++)
					p += sprintf(p, "%02x", ((unsigned char *)md5bin)[j]);
				p += sprintf(p, "] md5key[");
				for(j = 0; j < md5keylen; j++)
					p += sprintf(p, "%02x", ((unsigned char *)md5key)[j]);
				p += sprintf(p, "]" RETCODE);
				ShowNotice("%s\n", p);
#endif
			}
			return 1;
		}
		ShowInfo("auth ok %s %s" RETCODE, tmpstr, account->userid);
	}

/*
// do not remove this section. this is meant for future, and current forums usage
// as a login manager and CP for login server. [CLOWNISIUS]
	if (atoi(sql_row[10]) == 1) {
		return 4;
	}

	if (atoi(sql_row[10]) >= 5) {
		switch(atoi(sql_row[10])) {
		case 5:
			return 5;
			break;
		case 6:
			return 7;
			break;
		case 7:
			return 9;
			break;
		case 8:
			return 10;
			break;
		case 9:
			return 11;
			break;
		default:
			return 10;
			break;
		}
	}
*/
	ban_until_time = atol(sql_row[8]);

	//login {0-account_id/1-userid/2-user_pass/3-lastlogin/4-logincount/5-sex/6-connect_untl/7-last_ip/8-ban_until/9-state}
	if (ban_until_time != 0) { // if account is banned
		strftime(tmpstr, 20, date_format, localtime(&ban_until_time));
		tmpstr[19] = '\0';
		if (ban_until_time > time(NULL)) { // always banned
			return 6; // 6 = Your are Prohibited to log in until %s
		} else { // ban is finished
			// reset the ban time
			if (atoi(sql_row[9])==7) {//it was a temp ban - so we set STATE to 0
				sprintf(tmpsql, "UPDATE `%s` SET `ban_until`='0', `state`='0' WHERE %s `%s`='%s'", login_db, case_sensitive ? "BINARY" : "", login_db_userid, t_uid);
				strcpy(sql_row[9],"0"); //we clear STATE
			} else //it was a permanent ban + temp ban. So we leave STATE = 5, but clear the temp ban
				sprintf(tmpsql, "UPDATE `%s` SET `ban_until`='0' WHERE %s `%s`='%s'", login_db, case_sensitive ? "BINARY" : "", login_db_userid, t_uid);

			if (mysql_query(&mysql_handle, tmpsql)) {
				ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
				ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
			}
		}
	}

	if (atoi(sql_row[9])) {
		switch(atoi(sql_row[9])) { // packet 0x006a value + 1
		case 1:   // 0 = Unregistered ID
		case 2:   // 1 = Incorrect Password
		case 3:   // 2 = This ID is expired
		case 4:   // 3 = Rejected from Server
		case 5:   // 4 = You have been blocked by the GM Team
		case 6:   // 5 = Your Game's EXE file is not the latest version
		case 7:   // 6 = Your are Prohibited to log in until %s
		case 8:   // 7 = Server is jammed due to over populated
		case 9:   // 8 = No MSG (actually, all states after 9 except 99 are No MSG, use only this)
		case 100: // 99 = This ID has been totally erased
			ShowNotice("Auth Error #%d\n", atoi(sql_row[9]));
			return atoi(sql_row[9]) - 1;
			break;
		default:
			return 99; // 99 = ID has been totally erased
			break;
		}
	}

	if (atol(sql_row[6]) != 0 && atol(sql_row[6]) < time(NULL)) {
		return 2; // 2 = This ID is expired
	}

	if (online_check) {
		struct online_login_data* data = numdb_search(online_db,atol(sql_row[0]));
		unsigned char buf[8];
		if (data) {
			//Request char servers to kick this account out. [Skotlex]
			ShowWarning("User [%s] is already online - Rejected.\n",sql_row[1]);
			data->waiting_disconnect = 1;
			WBUFW(buf,0) = 0x2734;
			WBUFL(buf,2) = atol(sql_row[0]);
			charif_sendallwos(-1, buf, 6);
			add_timer(gettick()+30000, waiting_disconnect_timer, atol(sql_row[0]), 0);
			return 3; // Rejected
		}
	}

	account->account_id = atoi(sql_row[0]);
	account->login_id1 = rand();
	account->login_id2 = rand();
	memcpy(tmpstr, sql_row[3], 19);
	memcpy(account->lastlogin, tmpstr, 24);
	account->sex = sql_row[5][0] == 'S' ? 2 : sql_row[5][0]=='M';
	account->level = atoi(sql_row[10]) > 99 ? 99 : atoi(sql_row[10]); // as was in isGM() [zzo]

	if (account->sex != 2 && account->account_id < 700000)
		ShowWarning("Account %s has account id %d! Account IDs must be over 700000 to work properly!\n", account->userid, account->account_id);
	sprintf(tmpsql, "UPDATE `%s` SET `lastlogin` = NOW(), `logincount`=`logincount` +1, `last_ip`='%s'  WHERE %s  `%s` = '%s'",
	        login_db, ip, case_sensitive ? "BINARY" : "", login_db_userid, sql_row[1]);
	mysql_free_result(sql_res) ; //resource free
	if (mysql_query(&mysql_handle, tmpsql)) {
		ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
		ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
	}

	return -1;
}

//-----------------------------------------------------
// char-server packet parse
//-----------------------------------------------------
int parse_fromchar(int fd){
	int i, id;
	MYSQL_RES* sql_res;
	MYSQL_ROW  sql_row = NULL;

	unsigned char *p = (unsigned char *) &session[fd]->client_addr.sin_addr;
	char ip[16];

	sprintf(ip, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);

	for(id = 0; id < MAX_SERVERS; id++)
		if (server_fd[id] == fd)
			break;

	if (id == MAX_SERVERS)
		session[fd]->eof = 1;
	if(session[fd]->eof) {
		if (id < MAX_SERVERS) {
			ShowWarning("Char-server '%s' has disconnected.\n", server[id].name);
			server_fd[id] = -1;
			memset(&server[id], 0, sizeof(struct mmo_char_server));
			// server delete
			sprintf(tmpsql, "DELETE FROM `sstatus` WHERE `index`='%d'", id);
			// query
			if (mysql_query(&mysql_handle, tmpsql)) {
				ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
				ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
			}
		}
		do_close(fd);
		return 0;
	}

	while(RFIFOREST(fd) >= 2) {
//		printf("char_parse: %d %d packet case=%x\n", fd, RFIFOREST(fd), RFIFOW(fd, 0));

		switch (RFIFOW(fd,0)) {
		case 0x2709:
			if (log_login)
			{
				sprintf(tmpsql,"INSERT DELAYED INTO `%s`(`time`,`ip`,`user`,`rcode`,`log`) VALUES (NOW(), '%d.%d.%d.%d', '%s','%s', 'GM reload request')", loginlog_db, p[0], p[1], p[2], p[3], server[id].name, RETCODE);
				if (mysql_query(&mysql_handle, tmpsql)) {
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				}
			}
			read_gm_account();
			// send GM accounts to all char-servers
			send_GM_accounts(-1);
			RFIFOSKIP(fd,2);
			break;

		case 0x2712:
			if (RFIFOREST(fd) < 19)
				return 0;
		  {
			int account_id;
			account_id = RFIFOL(fd,2); // speed up
			for(i=0;i<AUTH_FIFO_SIZE;i++){
				if (auth_fifo[i].account_id == account_id &&
				    auth_fifo[i].login_id1 == RFIFOL(fd,6) &&
#if CMP_AUTHFIFO_LOGIN2 != 0
				    auth_fifo[i].login_id2 == RFIFOL(fd,10) && // relate to the versions higher than 18
#endif
				    auth_fifo[i].sex == RFIFOB(fd,14) &&
#if CMP_AUTHFIFO_IP != 0
				    auth_fifo[i].ip == RFIFOL(fd,15) &&
#endif
				    !auth_fifo[i].delflag) {
					auth_fifo[i].delflag = 1;
					ShowDebug("auth -> %d\n", i);
					break;
				}
			}

			if (i != AUTH_FIFO_SIZE) { // send account_reg2
				int p;
				time_t connect_until_time = 0;
				char email[40] = "";
				account_id=RFIFOL(fd,2);
				sprintf(tmpsql, "SELECT `email`,`connect_until` FROM `%s` WHERE `%s`='%d'", login_db, login_db_account_id, account_id);
				if (mysql_query(&mysql_handle, tmpsql)) {
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				}
				sql_res = mysql_store_result(&mysql_handle) ;
				if (sql_res) {
					sql_row = mysql_fetch_row(sql_res);
					connect_until_time = atol(sql_row[1]);
					strcpy(email, sql_row[0]);
				}
				mysql_free_result(sql_res);
				if (account_id > 0) {
					sprintf(tmpsql, "SELECT `str`,`value` FROM `%s` WHERE `type`='1' AND `account_id`='%d'",reg_db, account_id);
					if (mysql_query(&mysql_handle, tmpsql)) {
						ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
						ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
					}
					sql_res = mysql_store_result(&mysql_handle) ;
					if (sql_res) {
						WFIFOW(fd,0) = 0x2729;
						WFIFOL(fd,4) = account_id;
						for(p = 8; (sql_row = mysql_fetch_row(sql_res));p+=288){
							memcpy(WFIFOP(fd,p), sql_row[0], 32);
							memcpy(WFIFOP(fd,p+32),sql_row[1],256);
						}
						WFIFOW(fd,2) = p;
						WFIFOSET(fd,p);
						//printf("account_reg2 send : login->char (auth fifo)\n");
						WFIFOW(fd,0) = 0x2713;
						WFIFOL(fd,2) = account_id;
						WFIFOB(fd,6) = 0;
						memcpy(WFIFOP(fd, 7), email, 40);
						WFIFOL(fd,47) = (unsigned long) connect_until_time;
						WFIFOSET(fd,51);
					}
					mysql_free_result(sql_res);
				}
			} else {
				WFIFOW(fd,0) = 0x2713;
				WFIFOL(fd,2) = account_id;
				WFIFOB(fd,6) = 1;
				WFIFOSET(fd,51);
			}
			  }
			RFIFOSKIP(fd,19);
			break;

		case 0x2714:
			if (RFIFOREST(fd) < 6)
				return 0;
			// how many users on world? (update)
			if (server[id].users != RFIFOL(fd,2))
			{
				ShowStatus("set users %s : %d\n", server[id].name, RFIFOL(fd,2));

				server[id].users = RFIFOL(fd,2);
				sprintf(tmpsql,"UPDATE `sstatus` SET `user` = '%d' WHERE `index` = '%d'", server[id].users, id);
				// query
				if (mysql_query(&mysql_handle, tmpsql)) {
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				}
			}

			// send some answer
			WFIFOW(fd,0) = 0x2718;
			WFIFOSET(fd,2);

			RFIFOSKIP(fd,6);
			break;

		// We receive an e-mail/limited time request, because a player comes back from a map-server to the char-server
		case 0x2716:
			if (RFIFOREST(fd) < 6)
				return 0;
		  {
			int account_id;
			time_t connect_until_time = 0;
			char email[40] = "";
			account_id=RFIFOL(fd,2);
			sprintf(tmpsql,"SELECT `email`,`connect_until` FROM `%s` WHERE `%s`='%d'",login_db, login_db_account_id, account_id);
			if(mysql_query(&mysql_handle, tmpsql)) {
				ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
				ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
			}
			sql_res = mysql_store_result(&mysql_handle) ;
			if (sql_res) {
				sql_row = mysql_fetch_row(sql_res);
				connect_until_time = atol(sql_row[1]);
				strcpy(email, sql_row[0]);
			}
			mysql_free_result(sql_res);
			//printf("parse_fromchar: E-mail/limited time request from '%s' server (concerned account: %d)\n", server[id].name, RFIFOL(fd,2));
			WFIFOW(fd,0) = 0x2717;
			WFIFOL(fd,2) = RFIFOL(fd,2);
			memcpy(WFIFOP(fd, 6), email, 40);
			WFIFOL(fd,46) = (unsigned long) connect_until_time;
			WFIFOSET(fd,50);
		  }
			RFIFOSKIP(fd,6);
			break;

		case 0x2720:	// GM
			if (RFIFOREST(fd) < 4)
				return 0;
			if (RFIFOREST(fd) < RFIFOW(fd,2))
				return 0;
			//oldacc = RFIFOL(fd,4);
			ShowWarning("change GM isn't supported in this login server version.\n");
			ShowError("change GM error 0 %s\n", RFIFOP(fd, 8));

			RFIFOSKIP(fd, RFIFOW(fd, 2));
			WFIFOW(fd, 0) = 0x2721;
			WFIFOL(fd, 2) = RFIFOL(fd,4); // oldacc;
			WFIFOL(fd, 6) = 0; // newacc;
			WFIFOSET(fd, 10);
			return 0;

		// Map server send information to change an email of an account via char-server
		case 0x2722:	// 0x2722 <account_id>.L <actual_e-mail>.40B <new_e-mail>.40B
			if (RFIFOREST(fd) < 86)
				return 0;
		  {
			int acc;
			char actual_email[40], new_email[40];
			acc = RFIFOL(fd,2);
			memcpy(actual_email, RFIFOP(fd,6), 40);
			memcpy(new_email, RFIFOP(fd,46), 40);
			if (e_mail_check(actual_email) == 0)
				ShowWarning("Char-server '%s': Attempt to modify an e-mail on an account (@email GM command), but actual email is invalid (account: %d, ip: %s)" RETCODE,
				          server[id].name, acc, ip);
			else if (e_mail_check(new_email) == 0)
				ShowWarning("Char-server '%s': Attempt to modify an e-mail on an account (@email GM command) with a invalid new e-mail (account: %d, ip: %s)" RETCODE,
				          server[id].name, acc, ip);
			else if (strcmpi(new_email, "a@a.com") == 0)
				ShowWarning("Char-server '%s': Attempt to modify an e-mail on an account (@email GM command) with a default e-mail (account: %d, ip: %s)" RETCODE,
				          server[id].name, acc, ip);
			else {
				sprintf(tmpsql, "SELECT `%s`,`email` FROM `%s` WHERE `%s` = '%d'", login_db_userid, login_db, login_db_account_id, acc);
				if (mysql_query(&mysql_handle, tmpsql))
				{
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				}
				sql_res = mysql_store_result(&mysql_handle);
				if (sql_res) {
					sql_row = mysql_fetch_row(sql_res);	//row fetching

					if (strcmpi(sql_row[1], actual_email) == 0) {
						sprintf(tmpsql, "UPDATE `%s` SET `email` = '%s' WHERE `%s` = '%d'", login_db, new_email, login_db_account_id, acc);
						// query
						if (mysql_query(&mysql_handle, tmpsql)) {
							ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
							ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
						}
						ShowInfo("Char-server '%s': Modify an e-mail on an account (@email GM command) (account: %d (%s), new e-mail: %s, ip: %s)." RETCODE,
						      server[id].name, acc, sql_row[0], actual_email, ip);
					}
				}

			}
		  }
			RFIFOSKIP(fd, 86);
			break;

		case 0x2724:	// Receiving of map-server via char-server a status change resquest (by Yor)
			if (RFIFOREST(fd) < 10)
				return 0;
		  {
			int acc, statut;
			acc = RFIFOL(fd,2);
			statut = RFIFOL(fd,6);
			sprintf(tmpsql, "SELECT `state` FROM `%s` WHERE `%s` = '%d'", login_db, login_db_account_id, acc);
			if (mysql_query(&mysql_handle, tmpsql)) {
				ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
				ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
			}
			sql_res = mysql_store_result(&mysql_handle);
			if (sql_res) {
				sql_row = mysql_fetch_row(sql_res); // row fetching
			}
			if (atoi(sql_row[0]) != statut && statut != 0) {
				unsigned char buf[16];
				WBUFW(buf,0) = 0x2731;
				WBUFL(buf,2) = acc;
				WBUFB(buf,6) = 0; // 0: change of statut, 1: ban
				WBUFL(buf,7) = statut; // status or final date of a banishment
				charif_sendallwos(-1, buf, 11);
			}
			sprintf(tmpsql,"UPDATE `%s` SET `state` = '%d' WHERE `%s` = '%d'", login_db, statut,login_db_account_id,acc);
			//query
			if(mysql_query(&mysql_handle, tmpsql)) {
				ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
				ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
			}
			RFIFOSKIP(fd,10);
		  }
			return 0;

		case 0x2725: // Receiving of map-server via char-server a ban resquest (by Yor)
			if (RFIFOREST(fd) < 18)
				return 0;
		  {
			int acc;
			struct tm *tmtime;
			time_t timestamp, tmptime;
			acc = RFIFOL(fd,2);
			sprintf(tmpsql, "SELECT `ban_until` FROM `%s` WHERE `%s` = '%d'",login_db,login_db_account_id,acc);
			if (mysql_query(&mysql_handle, tmpsql)) {
				ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
				ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
			}
			sql_res = mysql_store_result(&mysql_handle);
			if (sql_res) {
				sql_row = mysql_fetch_row(sql_res); // row fetching
			}
			tmptime = atol(sql_row[0]);
			if (tmptime == 0 || tmptime < time(NULL))
				timestamp = time(NULL);
			else
				timestamp = tmptime;
			tmtime = localtime(&timestamp);
			tmtime->tm_year = tmtime->tm_year + (short)RFIFOW(fd,6);
			tmtime->tm_mon = tmtime->tm_mon + (short)RFIFOW(fd,8);
			tmtime->tm_mday = tmtime->tm_mday + (short)RFIFOW(fd,10);
			tmtime->tm_hour = tmtime->tm_hour + (short)RFIFOW(fd,12);
			tmtime->tm_min = tmtime->tm_min + (short)RFIFOW(fd,14);
			tmtime->tm_sec = tmtime->tm_sec + (short)RFIFOW(fd,16);
			timestamp = mktime(tmtime);
			if (timestamp != -1) {
				if (timestamp <= time(NULL))
					timestamp = 0;
				if (tmptime != timestamp) {
					if (timestamp != 0) {
						unsigned char buf[16];
						WBUFW(buf,0) = 0x2731;
						WBUFL(buf,2) = acc;
						WBUFB(buf,6) = 1; // 0: change of statut, 1: ban
						WBUFL(buf,7) = (unsigned int)timestamp; // status or final date of a banishment
						charif_sendallwos(-1, buf, 11);
					}
					ShowNotice("Account: %d Banned until: %ld\n", acc, timestamp);
					sprintf(tmpsql, "UPDATE `%s` SET `ban_until` = '%ld', `state`='7' WHERE `%s` = '%d'", login_db, (unsigned long)timestamp, login_db_account_id, acc);
					// query
					if (mysql_query(&mysql_handle, tmpsql)) {
						ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
						ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
					}
				}
			}
			RFIFOSKIP(fd,18);
			break;
		  }
			return 0;

		case 0x2727:
			if (RFIFOREST(fd) < 6)
				return 0;
		  {
				int acc,sex;
				unsigned char buf[16];
				acc=RFIFOL(fd,2);
				sprintf(tmpsql,"SELECT `sex` FROM `%s` WHERE `%s` = '%d'",login_db,login_db_account_id,acc);

			if(mysql_query(&mysql_handle, tmpsql)) {
				ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
				ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				return 0;
			}

	        sql_res = mysql_store_result(&mysql_handle) ;

	        if (sql_res)	{
		        if (mysql_num_rows(sql_res) == 0) {
				mysql_free_result(sql_res);
			        return 0;
			}
			sql_row = mysql_fetch_row(sql_res);	//row fetching
	        }

	        if (strcmpi(sql_row[0], "M") == 0)
                    sex = 1;
                else
                    sex = 0;
				sprintf(tmpsql,"UPDATE `%s` SET `sex` = '%c' WHERE `%s` = '%d'", login_db, (sex==0?'M':'F'), login_db_account_id, acc);
				//query
				if(mysql_query(&mysql_handle, tmpsql)) {
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				}
				WBUFW(buf,0) = 0x2723;
				WBUFL(buf,2) = acc;
				WBUFB(buf,6) = sex;
				charif_sendallwos(-1, buf, 7);
				RFIFOSKIP(fd,6);
			  }
			  return 0;

			case 0x2728:	// save account_reg
				if (RFIFOREST(fd) < 4 || RFIFOREST(fd) < RFIFOW(fd,2))
					return 0;
			  {
				int acc,p,j;
				char str[32];
				char temp_str[64]; //Needs twice as much space as the original string.
				char temp_str2[512];
				char value[256];
				acc=RFIFOL(fd,4);

				if (acc>0){
					unsigned char *buf;
					buf = (unsigned char*)aCalloc(RFIFOW(fd,2)+1, sizeof(unsigned char));
					for(p=8,j=0;p<RFIFOW(fd,2) && j<ACCOUNT_REG2_NUM;p+=288,j++){
						memcpy(str,RFIFOP(fd,p),32);
						memcpy(value,RFIFOP(fd,p+32),256);
						sprintf(tmpsql,"DELETE FROM `%s` WHERE `type`='1' AND `account_id`='%d' AND `str`='%s';",reg_db,acc,jstrescapecpy(temp_str,str));
						if(mysql_query(&mysql_handle, tmpsql)) {
							ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
							ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
						}
						sprintf(tmpsql,"INSERT INTO `%s` (`type`, `account_id`, `str`, `value`) VALUES ( 1 , '%d' , '%s' , '%s');",  reg_db, acc, jstrescapecpy(temp_str,str), jstrescapecpy(temp_str2,value));
						if(mysql_query(&mysql_handle, tmpsql)) {
							ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
							ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
						}
					}

					// Send to char
					memcpy(WBUFP(buf,0),RFIFOP(fd,0),RFIFOW(fd,2));
					WBUFW(buf,0)=0x2729;
					charif_sendallwos(fd,buf,WBUFW(buf,2));
					if (buf) aFree(buf);
				}
			  }
				RFIFOSKIP(fd,RFIFOW(fd,2));
				//printf("login: save account_reg (from char)\n");
			    break;

    case 0x272a:	// Receiving of map-server via char-server a unban resquest (by Yor)
			if (RFIFOREST(fd) < 6)
				return 0;
			{
				int acc;
				acc = RFIFOL(fd,2);
				sprintf(tmpsql,"SELECT `ban_until` FROM `%s` WHERE `%s` = '%d'",login_db,login_db_account_id,acc);
                if(mysql_query(&mysql_handle, tmpsql)) {
							ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
							ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
                }
                sql_res = mysql_store_result(&mysql_handle) ;
                if (sql_res)	{
                    sql_row = mysql_fetch_row(sql_res);	//row fetching
                }
				if (atol(sql_row[0]) != 0) {
				    sprintf(tmpsql,"UPDATE `%s` SET `ban_until` = '0', `state`='0' WHERE `%s` = '%d'", login_db,login_db_account_id,acc);
                    //query
                    if(mysql_query(&mysql_handle, tmpsql)) {
								ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
								ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
                    }
					break;
				}
				RFIFOSKIP(fd,6);
			}
			return 0;

    case 0x272b:    // Set account_id to online [Wizputer]
        if (RFIFOREST(fd) < 6)
            return 0;
        add_online_user(id, RFIFOL(fd,2));
        RFIFOSKIP(fd,6);
        break;

    case 0x272c:   // Set account_id to offline [Wizputer]
        if (RFIFOREST(fd) < 6)
            return 0;
        remove_online_user(RFIFOL(fd,2));
        RFIFOSKIP(fd,6);
        break;

	default:
		ShowError("login: unknown packet %x! (from char).\n", RFIFOW(fd,0));
		session[fd]->eof = 1;
	return 0;
	}
	}

	return 0;
}

//Lan ip check added by Kashy
int lan_ip_check(unsigned char *p) {
	int y;
	int lancheck = 1;
	int lancharip[4];

	unsigned int k0, k1, k2, k3;
	sscanf(lan_char_ip, "%d.%d.%d.%d", &k0, &k1, &k2, &k3);
	lancharip[0] = k0; lancharip[1] = k1; lancharip[2] = k2; lancharip[3] = k3;

	for(y = 0; y < 4; y++) {
		if ((lancharip[y] & subnetmaski[y])!= (p[y]))
		lancheck = 0;
		break; }

	ShowInfo("LAN check: "CL_CYAN"%s"CL_RESET".\n", (lancheck) ? "LAN" : "WAN");
	return lancheck;
}

//----------------------------------------------------------------------------------------
// Default packet parsing (normal players or administation/char-server connection requests)
//----------------------------------------------------------------------------------------
int parse_login(int fd) {
	//int len;

	MYSQL_RES* sql_res ;
	MYSQL_ROW  sql_row = NULL;

	char t_uid[100];
	//int sql_fields, sql_cnt;
	struct mmo_account account;

	int result, i;
	unsigned char *p = (unsigned char *) &session[fd]->client_addr.sin_addr;
	char ip[16];

	sprintf(ip, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);

        memset(&account, 0, sizeof(account));

	if (ipban > 0) {
		//ip ban
		//p[0], p[1], p[2], p[3]
		//request DB connection
		//check
		sprintf(tmpsql, "SELECT count(*) FROM `ipbanlist` WHERE `list` = '%d.*.*.*' OR `list` = '%d.%d.*.*' OR `list` = '%d.%d.%d.*' OR `list` = '%d.%d.%d.%d'",
		  p[0], p[0], p[1], p[0], p[1], p[2], p[0], p[1], p[2], p[3]);
		if (mysql_query(&mysql_handle, tmpsql)) {
			ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
			ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
			// close connection because we can't verify their connectivity.
			session[fd]->eof = 1;
		} else { //Avoid entering as it causes a crash.
			sql_res = mysql_store_result(&mysql_handle) ;
			sql_row = mysql_fetch_row(sql_res);	//row fetching

			if (atoi(sql_row[0]) >0) {
				// ip ban ok.
				ShowWarning("packet from banned ip : %d.%d.%d.%d\n" RETCODE, p[0], p[1], p[2], p[3]);
				if (log_login)
				{
					sprintf(tmpsql,"INSERT DELAYED INTO `%s`(`time`,`ip`,`user`,`rcode`,`log`) VALUES (NOW(), '%d.%d.%d.%d', 'unknown','-3', 'ip banned')", loginlog_db, p[0], p[1], p[2], p[3]);

					// query
					if(mysql_query(&mysql_handle, tmpsql)) {
						ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
						ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
					}
				}
				ShowInfo ("close session connection...\n");

				// close connection
				session[fd]->eof = 1;

			} else {
				ShowInfo ("packet from ip (ban check ok) : %d.%d.%d.%d" RETCODE, p[0], p[1], p[2], p[3]);
			}
			mysql_free_result(sql_res);
		}
	}
	if (session[fd]->eof) {
		for(i = 0; i < MAX_SERVERS; i++)
			if (server_fd[i] == fd)
				server_fd[i] = -1;
		do_close(fd);
		return 0;
	}

	while(RFIFOREST(fd)>=2){
		ShowDebug("parse_login : %d %d packet case=%x\n", fd, RFIFOREST(fd), RFIFOW(fd,0));

		switch(RFIFOW(fd,0)){
		case 0x200:		// New alive packet: structure: 0x200 <account.userid>.24B. used to verify if client is always alive.
			if (RFIFOREST(fd) < 26)
				return 0;
			RFIFOSKIP(fd,26);
			break;

		case 0x204:		// New alive packet: structure: 0x204 <encrypted.account.userid>.16B. (new ragexe from 22 june 2004)
			if (RFIFOREST(fd) < 18)
				return 0;
			RFIFOSKIP(fd,18);
			break;

		case 0x64:		// request client login
		case 0x01dd:	// request client login with encrypt
			if((int)RFIFOREST(fd)< ((RFIFOW(fd, 0) ==0x64)?55:47))
				return 0;

			ShowInfo("client connection request %s from %d.%d.%d.%d\n", RFIFOP(fd, 6), p[0], p[1], p[2], p[3]);
			account.version = RFIFOL(fd, 2);
			if (!account.version) account.version = 1;
			account.userid = (char*)RFIFOP(fd, 6);
			account.userid[23] = '\0';
			account.passwd = (char*)RFIFOP(fd, 30);
			account.passwd[23] = '\0';
#ifdef PASSWORDENC
			account.passwdenc= (RFIFOW(fd,0)==0x64)?0:PASSWORDENC;
#else
			account.passwdenc=0;
#endif
			result=mmo_auth(&account, fd);


		jstrescapecpy(t_uid,(char*)RFIFOP(fd, 6));
		if(result==-1){
		// as we have queried account level earlier in mmo_auth anyway, no need to do this again [zzo]
//		    int gm_level = isGM(account.account_id); // removed by [zzo]

        	    if (min_level_to_connect > account.level) {
					WFIFOW(fd,0) = 0x81;
					WFIFOB(fd,2) = 1; // 01 = Server closed
					WFIFOSET(fd,3);
		    } else {

                    if (p[0] != 127 && log_login) {
                         sprintf(tmpsql,"INSERT DELAYED INTO `%s`(`time`,`ip`,`user`,`rcode`,`log`) VALUES (NOW(), '%d.%d.%d.%d', '%s','100', 'login ok')", loginlog_db, p[0], p[1], p[2], p[3], t_uid);
                         //query
                         if(mysql_query(&mysql_handle, tmpsql)) {
										ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
										ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
                         }
                    }
                    if (account.level)
						ShowStatus("Connection of the GM (level:%d) account '%s' accepted.\n", account.level, account.userid);
					else
						ShowStatus("Connection of the account '%s' accepted.\n", account.userid);
                    server_num=0;
				for(i = 0; i < MAX_SERVERS; i++) {
					if (server_fd[i] >= 0) {
						//Lan check added by Kashy
						if (lan_ip_check(p))
							WFIFOL(fd,47+server_num*32) = inet_addr(lan_char_ip);
						else
                            WFIFOL(fd,47+server_num*32) = server[i].ip;
                            WFIFOW(fd,47+server_num*32+4) = server[i].port;
                            memcpy(WFIFOP(fd,47+server_num*32+6), server[i].name, 20);
                            WFIFOW(fd,47+server_num*32+26) = server[i].users;
                            WFIFOW(fd,47+server_num*32+28) = server[i].maintenance;
                            WFIFOW(fd,47+server_num*32+30) = server[i].new_;
                            server_num++;
                        }
                    }
                    // if at least 1 char-server
                    if (server_num > 0) {
                        WFIFOW(fd,0)=0x69;
                        WFIFOW(fd,2)=47+32*server_num;
                        WFIFOL(fd,4)=account.login_id1;
                        WFIFOL(fd,8)=account.account_id;
                        WFIFOL(fd,12)=account.login_id2;
                        WFIFOL(fd,16)=0;
                        memcpy(WFIFOP(fd,20),account.lastlogin,24);
                        WFIFOB(fd,46)=account.sex;
                        WFIFOSET(fd,47+32*server_num);
                        if(auth_fifo_pos>=AUTH_FIFO_SIZE)
                            auth_fifo_pos=0;
                        auth_fifo[auth_fifo_pos].account_id=account.account_id;
                        auth_fifo[auth_fifo_pos].login_id1=account.login_id1;
                        auth_fifo[auth_fifo_pos].login_id2=account.login_id2;
                        auth_fifo[auth_fifo_pos].sex=account.sex;
                        auth_fifo[auth_fifo_pos].delflag=0;
                        auth_fifo[auth_fifo_pos].ip = session[fd]->client_addr.sin_addr.s_addr;
                        auth_fifo_pos++;
                    } else {
                        WFIFOW(fd,0) = 0x81;
                        WFIFOB(fd,2) = 1; // 01 = Server closed
                        WFIFOSET(fd,3);
                    }
            }
      } else {
		char tmp_sql[512];
		char error[64];
		if (log_login)
		{
			sprintf(tmp_sql,"INSERT DELAYED INTO `%s`(`time`,`ip`,`user`,`rcode`,`log`) VALUES (NOW(), '%d.%d.%d.%d', '%s', '%d','login failed : %%s')", loginlog_db, p[0], p[1], p[2], p[3], t_uid, result);
			switch((result + 1)) {
			case -2:  //-3 = Account Banned
				sprintf(tmpsql,tmp_sql,"Account banned.");
				sprintf(error,"Account banned.");
			break;
			case -1:  //-2 = Dynamic Ban
				sprintf(tmpsql,tmp_sql,"dynamic ban (ip and account).");
				sprintf(error,"dynamic ban (ip and account).");
			break;
			case 1:   // 0 = Unregistered ID
				sprintf(tmpsql,tmp_sql,"Unregisterd ID.");
				sprintf(error,"Unregisterd ID.");
			break;
			case 2:   // 1 = Incorrect Password
				sprintf(tmpsql,tmp_sql,"Incorrect Password.");
				sprintf(error,"Incorrect Password.");
			break;
			case 3:   // 2 = This ID is expired
				sprintf(tmpsql,tmp_sql,"Account Expired.");
				sprintf(error,"Account Expired.");
			break;
			case 4:   // 3 = Rejected from Server
				sprintf(tmpsql,tmp_sql,"Rejected from server.");
				sprintf(error,"Rejected from server.");
			break;
			case 5:   // 4 = You have been blocked by the GM Team
				sprintf(tmpsql,tmp_sql,"Blocked by GM.");
				sprintf(error,"Blocked by GM.");
			break;
			case 6:   // 5 = Your Game's EXE file is not the latest version
				sprintf(tmpsql,tmp_sql,"Not latest game EXE.");
				sprintf(error,"Not latest game EXE.");
			break;
			case 7:   // 6 = Your are Prohibited to log in until %s
				sprintf(tmpsql,tmp_sql,"Banned.");
				sprintf(error,"Banned.");
			break;
			case 8:   // 7 = Server is jammed due to over populated
				sprintf(tmpsql,tmp_sql,"Server Over-population.");
				sprintf(error,"Server Over-population.");
			break;
			case 9:   // 8 = No MSG (actually, all states after 9 except 99 are No MSG, use only this)
				sprintf(tmpsql,tmp_sql," ");
				sprintf(error," ");
			break;
			case 100: // 99 = This ID has been totally erased
				sprintf(tmpsql,tmp_sql,"Account gone.");
				sprintf(error,"Account gone.");
			break;
			default:
				sprintf(tmpsql,tmp_sql,"Uknown Error.");
				sprintf(error,"Uknown Error.");
			break;
				}
				//query
				if(mysql_query(&mysql_handle, tmpsql)) {
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				}
			} //End login log of error.
			if ((result == 1) && (dynamic_pass_failure_ban != 0) && log_login){	// failed password
				sprintf(tmpsql,"SELECT count(*) FROM `%s` WHERE `ip` = '%d.%d.%d.%d' AND `rcode` = '1' AND `time` > NOW() - INTERVAL %d MINUTE",
				  loginlog_db, p[0], p[1], p[2], p[3], dynamic_pass_failure_ban_time);	//how many times filed account? in one ip.
				if(mysql_query(&mysql_handle, tmpsql)) {
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				}
				//check query result
				sql_res = mysql_store_result(&mysql_handle) ;
				sql_row = mysql_fetch_row(sql_res);	//row fetching

				if (atoi(sql_row[0]) >= dynamic_pass_failure_ban_how_many ) {
					sprintf(tmpsql,"INSERT INTO `ipbanlist`(`list`,`btime`,`rtime`,`reason`) VALUES ('%d.%d.%d.*', NOW() , NOW() +  INTERVAL %d MINUTE ,'Password error ban: %s')", p[0], p[1], p[2], dynamic_pass_failure_ban_how_long, t_uid);
					if(mysql_query(&mysql_handle, tmpsql)) {
						ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
						ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
					}
				}
				mysql_free_result(sql_res);
			}
			else if (result == -2){	//dynamic banned - add ip to ban list.
				sprintf(tmpsql,"INSERT INTO `ipbanlist`(`list`,`btime`,`rtime`,`reason`) VALUES ('%d.%d.%d.*', NOW() , NOW() +  INTERVAL 1 MONTH ,'Dynamic banned user id : %s')", p[0], p[1], p[2], t_uid);
				if(mysql_query(&mysql_handle, tmpsql)) {
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
				}
				result = -3;
			}else if(result == 6){ //not lastet version ..
				//result = 5;
			}

			sprintf(tmpsql,"SELECT `ban_until` FROM `%s` WHERE %s `%s` = '%s'",login_db, case_sensitive ? "BINARY" : "",login_db_userid, t_uid);
            if(mysql_query(&mysql_handle, tmpsql)) {
					ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
					ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
            }
            sql_res = mysql_store_result(&mysql_handle) ;
            if (sql_res)	{
                sql_row = mysql_fetch_row(sql_res);	//row fetching
            }
			//cannot connect login failed
			memset(WFIFOP(fd,0),'\0',23);
			WFIFOW(fd,0)=0x6a;
			WFIFOB(fd,2)=result;
			if (result == 6) { // 6 = Your are Prohibited to log in until %s
				if (atol(sql_row[0]) != 0) { // if account is banned, we send ban timestamp
					char tmpstr[256];
					time_t ban_until_time;
					ban_until_time = atol(sql_row[0]);
					strftime(tmpstr, 20, date_format, localtime(&ban_until_time));
					tmpstr[19] = '\0';
					memcpy(WFIFOP(fd,3), tmpstr, 20);
				} else { // we send error message
					memcpy(WFIFOP(fd,3), error, 20);
				}
			}
			WFIFOSET(fd,23);
		}
		RFIFOSKIP(fd,(RFIFOW(fd,0)==0x64)?55:47);
		break;

	case 0x01db:	// request password key
		if (session[fd]->session_data) {
			ShowWarning("login: abnormal request of MD5 key (already opened session).\n");
			session[fd]->eof = 1;
			return 0;
		}
		ShowDebug("Request Password key -%s\n",md5key);
		RFIFOSKIP(fd,2);
		WFIFOW(fd,0)=0x01dc;
		WFIFOW(fd,2)=4+md5keylen;
		memcpy(WFIFOP(fd,4),md5key,md5keylen);
		WFIFOSET(fd,WFIFOW(fd,2));
		break;

	case 0x2710:	// request Char-server connection
				if(RFIFOREST(fd)<86)
					return 0;
				{
					unsigned char* server_name;
					if (log_login)
					{
						sprintf(tmpsql,"INSERT DELAYED INTO `%s`(`time`,`ip`,`user`,`rcode`,`log`) VALUES (NOW(), '%d.%d.%d.%d', '%s@%s','100', 'charserver - %s@%d.%d.%d.%d:%d')", loginlog_db, p[0], p[1], p[2], p[3], RFIFOP(fd, 2),RFIFOP(fd, 60),RFIFOP(fd, 60), RFIFOB(fd, 54), RFIFOB(fd, 55), RFIFOB(fd, 56), RFIFOB(fd, 57), RFIFOW(fd, 58));

						//query
						if(mysql_query(&mysql_handle, tmpsql)) {
							ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
							ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
						}
					}
					ShowInfo("server connection request %s @ %d.%d.%d.%d:%d (%d.%d.%d.%d)\n",
						RFIFOP(fd, 60), RFIFOB(fd, 54), RFIFOB(fd, 55), RFIFOB(fd, 56), RFIFOB(fd, 57), RFIFOW(fd, 58),
						p[0], p[1], p[2], p[3]);
				account.userid = (char*)RFIFOP(fd, 2);
				account.userid[23] = '\0';
				account.passwd = (char*)RFIFOP(fd, 26);
				account.passwd[23] = '\0';
				account.passwdenc = 0;
				server_name = RFIFOP(fd,60);
				result = mmo_auth(&account, fd);
		//printf("Result: %d - Sex: %d - Account ID: %d\n",result,account.sex,(int) account.account_id);

				if(result == -1 && account.sex==2 && account.account_id<MAX_SERVERS && server_fd[account.account_id]==-1){
				    ShowStatus("Connection of the char-server '%s' accepted.\n", server_name);
			        memset(&server[account.account_id], 0, sizeof(struct mmo_char_server));
					server[account.account_id].ip=RFIFOL(fd,54);
					server[account.account_id].port=RFIFOW(fd,58);
					memcpy(server[account.account_id].name,RFIFOP(fd,60),20);
					server[account.account_id].users=0;
					server[account.account_id].maintenance=RFIFOW(fd,82);
					server[account.account_id].new_=RFIFOW(fd,84);
					server_fd[account.account_id]=fd;
					sprintf(tmpsql,"DELETE FROM `sstatus` WHERE `index`='%ld'", account.account_id);
					//query
					if(mysql_query(&mysql_handle, tmpsql)) {
						ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
						ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
					}

					jstrescapecpy(t_uid,server[account.account_id].name);
					sprintf(tmpsql,"INSERT INTO `sstatus`(`index`,`name`,`user`) VALUES ( '%ld', '%s', '%d')",
						account.account_id, server[account.account_id].name,0);
					//query
					if(mysql_query(&mysql_handle, tmpsql)) {
						ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
						ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
					}
					WFIFOW(fd,0)=0x2711;
					WFIFOB(fd,2)=0;
					WFIFOSET(fd,3);
					session[fd]->func_parse=parse_fromchar;
					realloc_fifo(fd,FIFOSIZE_SERVERLINK,FIFOSIZE_SERVERLINK);
					// send GM account to char-server
					send_GM_accounts(fd);
				} else {
					WFIFOW(fd, 0) =0x2711;
					WFIFOB(fd, 2)=3;
					WFIFOSET(fd, 3);
				}
	  }
				RFIFOSKIP(fd, 86);
				return 0;

		case 0x7530:	// request Athena information
			WFIFOW(fd,0)=0x7531;
			WFIFOB(fd,2)=ATHENA_MAJOR_VERSION;
			WFIFOB(fd,3)=ATHENA_MINOR_VERSION;
			WFIFOB(fd,4)=ATHENA_REVISION;
			WFIFOB(fd,5)=ATHENA_RELEASE_FLAG;
			WFIFOB(fd,6)=ATHENA_OFFICIAL_FLAG;
			WFIFOB(fd,7)=ATHENA_SERVER_LOGIN;
			WFIFOW(fd,8)=ATHENA_MOD_VERSION;
			WFIFOSET(fd,10);
			RFIFOSKIP(fd,2);
			ShowInfo ("Athena version check...\n");
			break;

		case 0x7532:
		default:
			ShowStatus ("End of connection (ip: %s)" RETCODE, ip);
			session[fd]->eof = 1;
			return 0;
		}
	}

	return 0;
}

// Console Command Parser [Wizputer]
int parse_console(char *buf) {
    char *type,*command;

    type = (char *)aMalloc(64);
    command = (char *)aMalloc(64);

    memset(type,0,64);
    memset(command,0,64);

    ShowInfo("Console: %s\n",buf);

    if ( sscanf(buf, "%[^:]:%[^\n]", type , command ) < 2 )
        sscanf(buf,"%[^\n]",type);

    ShowInfo("Type of command: %s || Command: %s \n",type,command);

    if(buf) aFree(buf);
    if(type) aFree(type);
    if(command) aFree(command);

    return 0;
}

//-------------------------------------------------
// Return numerical value of a switch configuration
// on/off, english, fran�ais, deutsch, espa�ol
//-------------------------------------------------
int config_switch(const char *str) {
	if (strcmpi(str, "on") == 0 || strcmpi(str, "yes") == 0 || strcmpi(str, "oui") == 0 || strcmpi(str, "ja") == 0 || strcmpi(str, "si") == 0)
		return 1;
	if (strcmpi(str, "off") == 0 || strcmpi(str, "no") == 0 || strcmpi(str, "non") == 0 || strcmpi(str, "nein") == 0)
		return 0;

	return atoi(str);
}


//Lan Support conf reading added by Kashy
int login_lan_config_read(const char *lancfgName){
	int i;
	char subnetmask[128];
	char line[1024], w1[1024], w2[1024];
	FILE *fp;

	fp=fopen(lancfgName, "r");

	if (fp == NULL) {
		ShowError("file not found: %s\n", lancfgName);
		return 1;
	}
	ShowInfo("reading configuration file %s...\n", lancfgName);
	while(fgets(line, sizeof(line)-1, fp)){
		if (line[0] == '/' && line[1] == '/')
			continue;

		i = sscanf(line,"%[^:]: %[^\r\n]",w1,w2);
		if(i!=2)
			continue;

		else if(strcmpi(w1,"lan_char_ip")==0){
			strcpy(lan_char_ip, w2);
			ShowStatus("set Lan_Char_IP : %s\n",w2);
			}

		else if(strcmpi(w1,"subnetmask")==0){
			unsigned int k0, k1, k2, k3;

			strcpy(subnetmask, w2);
			sscanf(subnetmask, "%d.%d.%d.%d", &k0, &k1, &k2, &k3);
			subnetmaski[0] = k0; subnetmaski[1] = k1; subnetmaski[2] = k2; subnetmaski[3] = k3;
			ShowStatus("set subnetmask : %s\n",w2);
			}
		}
	fclose(fp);

	{
		unsigned int a0, a1, a2, a3;
		unsigned char p[4];
		sscanf(lan_char_ip, "%d.%d.%d.%d", &a0, &a1, &a2, &a3);
		p[0] = a0; p[1] = a1; p[2] = a2; p[3] = a3;
		ShowInfo("LAN test of LAN IP of the char-server:\n");
		if (lan_ip_check(p) == 0) {
			ShowError("\033[1;31m LAN IP of the char-server doesn't belong to the specified Sub-network\033[0m\n");
		}
	}

	ShowInfo("Finished reading %s.\n",lancfgName);

	return 0;
}

//-----------------------------------------------------
//BANNED IP CHECK.
//-----------------------------------------------------
int ip_ban_check(int tid, unsigned int tick, int id, int data){

	//query
	if(mysql_query(&mysql_handle, "DELETE FROM `ipbanlist` WHERE `rtime` <= NOW()")) {
		ShowSQL("DB error - %s\n",mysql_error(&mysql_handle));
		ShowDebug("at %s:%d - %s\n", __FILE__,__LINE__,tmp_sql);
	}

	return 0;
}

//-----------------------------------------------------
// reading configuration
//-----------------------------------------------------
int login_config_read(const char *cfgName){
	int i;
	struct hostent *h = NULL;
	char line[1024], w1[1024], w2[1024];
	FILE *fp;

	fp=fopen(cfgName,"r");

	if(fp==NULL){
		ShowError("Configuration file (%s) not found.\n", cfgName);
		return 1;
	}
	ShowInfo("reading configuration file %s...\n", cfgName);
	while(fgets(line, sizeof(line)-1, fp)){
		if(line[0] == '/' && line[1] == '/')
			continue;

		i=sscanf(line,"%[^:]: %[^\r\n]",w1,w2);
		if(i!=2)
			continue;

		remove_control_chars((unsigned char *) w1);
		remove_control_chars((unsigned char *) w2);
		if(strcmpi(w1,"timestamp_format") == 0) {
			strncpy(timestamp_format, w2, 20);
		} else if(strcmpi(w1,"console_silent")==0){
			msg_silent = 0; //To always allow the next line to show up.
			ShowInfo("Console Silent Setting: %d\n", atoi(w2));
			msg_silent = atoi(w2);
		} else if (strcmpi(w1, "bind_ip") == 0) {
			bind_ip_set_ = 1;
			h = gethostbyname (w2);
			if (h != NULL) {
				ShowStatus("Login server binding IP address : %s -> %d.%d.%d.%d\n", w2, (unsigned char)h->h_addr[0], (unsigned char)h->h_addr[1], (unsigned char)h->h_addr[2], (unsigned char)h->h_addr[3]);
				sprintf(bind_ip_str, "%d.%d.%d.%d", (unsigned char)h->h_addr[0], (unsigned char)h->h_addr[1], (unsigned char)h->h_addr[2], (unsigned char)h->h_addr[3]);
			} else
				memcpy(bind_ip_str,w2,16);
		} else if(strcmpi(w1,"login_port")==0){
			login_port=atoi(w2);
			ShowStatus("set login_port : %s\n",w2);
		}
		else if(strcmpi(w1,"ipban")==0){
			ipban=atoi(w2);
			ShowStatus("set ipban : %d\n",ipban);
		}
		//account ban -> ip ban
		else if(strcmpi(w1,"dynamic_account_ban")==0){
			dynamic_account_ban=atoi(w2);
			ShowStatus("set dynamic_account_ban : %d\n",dynamic_account_ban);
		}
		else if(strcmpi(w1,"dynamic_account_ban_class")==0){
			dynamic_account_ban_class=atoi(w2);
			ShowStatus("set dynamic_account_ban_class : %d\n",dynamic_account_ban_class);
		}
		//dynamic password error ban
		else if(strcmpi(w1,"dynamic_pass_failure_ban")==0){
			dynamic_pass_failure_ban=atoi(w2);
			ShowStatus("set dynamic_pass_failure_ban : %d\n",dynamic_pass_failure_ban);
		}
		else if(strcmpi(w1,"dynamic_pass_failure_ban_time")==0){
			dynamic_pass_failure_ban_time=atoi(w2);
			ShowStatus("set dynamic_pass_failure_ban_time : %d\n",dynamic_pass_failure_ban_time);
		}
		else if(strcmpi(w1,"dynamic_pass_failure_ban_how_many")==0){
			dynamic_pass_failure_ban_how_many=atoi(w2);
			ShowStatus("set dynamic_pass_failure_ban_how_many : %d\n",dynamic_pass_failure_ban_how_many);
		}
		else if(strcmpi(w1,"dynamic_pass_failure_ban_how_long")==0){
			dynamic_pass_failure_ban_how_long=atoi(w2);
			ShowStatus("set dynamic_pass_failure_ban_how_long : %d\n",dynamic_pass_failure_ban_how_long);
		} else if(strcmpi(w1, "new_account") == 0){ 	//Added by Sirius for new account _M/_F
			new_account_flag = atoi(w2);		//Added by Sirius for new account _M/_F
		} else if(strcmpi(w1, "check_client_version") == 0){ 		//Added by Sirius for client version check
			//check_client_version = config_switch(w2); 		//Added by Sirius for client version check
			if(strcmpi(w2,"on") == 0 || strcmpi(w2,"yes") == 0 ){
				check_client_version = 1;
			} else if(strcmpi(w2,"off") == 0 || strcmpi(w2,"no") == 0 ){
				check_client_version = 0;
			}
		} else if(strcmpi(w1, "client_version_to_connect") == 0){	//Added by Sirius for client version check
			client_version_to_connect = atoi(w2);			//Added by SIrius for client version check
		} else if(strcmpi(w1,"use_MD5_passwords")==0){
			if (!strcmpi(w2,"yes")) {
				use_md5_passwds=1;
			} else if (!strcmpi(w2,"no")){
				use_md5_passwds=0;
			}
			ShowStatus("Using MD5 Passwords: %s \n",w2);
		}
        else if (strcmpi(w1, "date_format") == 0) { // note: never have more than 19 char for the date!
				switch (atoi(w2)) {
				case 0:
					strcpy(date_format, "%d-%m-%Y %H:%M:%S"); // 31-12-2004 23:59:59
					break;
				case 1:
					strcpy(date_format, "%m-%d-%Y %H:%M:%S"); // 12-31-2004 23:59:59
					break;
				case 2:
					strcpy(date_format, "%Y-%d-%m %H:%M:%S"); // 2004-31-12 23:59:59
					break;
				case 3:
					strcpy(date_format, "%Y-%m-%d %H:%M:%S"); // 2004-12-31 23:59:59
					break;
				}
		}
        else if (strcmpi(w1, "min_level_to_connect") == 0) {
				min_level_to_connect = atoi(w2);
		}
        else if (strcmpi(w1, "check_ip_flag") == 0) {
                check_ip_flag = config_switch(w2);
    	}
    	else if (strcmpi(w1, "console") == 0) {
			    if(strcmpi(w2,"on") == 0 || strcmpi(w2,"yes") == 0 )
			        console = 1;
        }
    	else if (strcmpi(w1, "case_sensitive") == 0) {
			if(strcmpi(w2,"on") == 0 || strcmpi(w2,"yes") == 0 )
				case_sensitive = 1;
			if(strcmpi(w2,"off") == 0 || strcmpi(w2,"no") == 0 )
				case_sensitive = 0;
			else
				case_sensitive = atoi(w2);
		} else if (strcmpi(w1, "allowed_regs") == 0) { //account flood protection system [Kevin]
			allowed_regs = atoi(w2);
		} else if (strcmpi(w1, "time_allowed") == 0) {
			time_allowed = atoi(w2);
		} else if (strcmpi(w1, "online_check") == 0) {
			if(strcmpi(w2,"on") == 0 || strcmpi(w2,"yes") == 0 )
				online_check = 1;
			else if(strcmpi(w2,"off") == 0 || strcmpi(w2,"no") == 0 )
				online_check = 0;
			else
				online_check = atoi(w2);
		} else if (strcmpi(w1, "log_login") == 0) {
			if(strcmpi(w2,"on") == 0 || strcmpi(w2,"yes") == 0 )
				log_login = 1;
			else if(strcmpi(w2,"off") == 0 || strcmpi(w2,"no") == 0 )
				log_login = 0;
			else
				log_login = atoi(w2);
		} else if (strcmpi(w1, "import") == 0) {
			login_config_read(w2);
		}
 	}
	fclose(fp);
	ShowInfo("done reading %s.\n", cfgName);
	return 0;
}

void sql_config_read(const char *cfgName){ /* Kalaspuff, to get login_db */
	int i;
	char line[1024], w1[1024], w2[1024];
	FILE *fp=fopen(cfgName,"r");
	if(fp==NULL){
		ShowFatalError("file not found: %s\n",cfgName);
		exit(1);
	}
	ShowInfo("reading configuration file %s...\n", cfgName);
	while(fgets(line, sizeof(line)-1, fp)){
		if(line[0] == '/' && line[1] == '/')
			continue;
		i=sscanf(line,"%[^:]: %[^\r\n]",w1,w2);
		if(i!=2)
			continue;
		if (strcmpi(w1, "login_db") == 0) {
			strcpy(login_db, w2);
		}
		//add for DB connection
		else if(strcmpi(w1,"login_server_ip")==0){
			strcpy(login_server_ip, w2);
			ShowStatus ("set login_server_ip : %s\n",w2);
		}
		else if(strcmpi(w1,"login_server_port")==0){
			login_server_port=atoi(w2);
			ShowStatus ("set login_server_port : %s\n",w2);
		}
		else if(strcmpi(w1,"login_server_id")==0){
			strcpy(login_server_id, w2);
			ShowStatus ("set login_server_id : %s\n",w2);
		}
		else if(strcmpi(w1,"login_server_pw")==0){
			strcpy(login_server_pw, w2);
			ShowStatus ("set login_server_pw : %s\n",w2);
		}
		else if(strcmpi(w1,"login_server_db")==0){
			strcpy(login_server_db, w2);
			ShowStatus ("set login_server_db : %s\n",w2);
		}
		else if(strcmpi(w1,"default_codepage")==0){
			strcpy(default_codepage, w2);
			ShowStatus ("set default_codepage : %s\n",w2);
		}
		//added for custom column names for custom login table
		else if(strcmpi(w1,"login_db_account_id")==0){
			strcpy(login_db_account_id, w2);
		}
		else if(strcmpi(w1,"login_db_userid")==0){
			strcpy(login_db_userid, w2);
		}
		else if(strcmpi(w1,"login_db_user_pass")==0){
			strcpy(login_db_user_pass, w2);
		}
		else if(strcmpi(w1,"login_db_level")==0){
			strcpy(login_db_level, w2);
		}
		else if (strcmpi(w1, "loginlog_db") == 0) {
			strcpy(loginlog_db, w2);
		}
		else if (strcmpi(w1, "lowest_gm_level") == 0) {
			lowest_gm_level = atoi(w2);
		}
		else if (strcmpi(w1, "reg_db") == 0) {
			strcpy(reg_db, w2);
		}
		//support the import command, just like any other config
		else if(strcmpi(w1,"import")==0){
			sql_config_read(w2);
		}
	}
	fclose(fp);
	ShowInfo("done reading %s.\n", cfgName);
}

//--------------------------------------
// Function called at exit of the server
//--------------------------------------
void do_final(void) {
	//sync account when terminating.
	//but no need when you using DBMS (mysql)
	mmo_db_close();
	numdb_final(online_db, online_db_final);
	if (gm_account_db)
		aFree(gm_account_db);
}

void set_server_type(void)
{
	SERVER_TYPE = ATHENA_SERVER_LOGIN;
}
int do_init(int argc,char **argv){
	//initialize login server
	int i;

	//read login configue
	login_config_read( (argc>1)?argv[1]:LOGIN_CONF_NAME );
	sql_config_read(SQL_CONF_NAME);
	login_lan_config_read((argc > 1) ? argv[1] : LAN_CONF_NAME);
	//Generate Passworded Key.
	ShowInfo("Initializing md5key...\n");
	memset(md5key, 0, sizeof(md5key));
	md5keylen=rand()%4+12;
	for(i=0;i<md5keylen;i++)
		md5key[i]=rand()%255+1;
	ShowInfo("md5key setup complete\n");


	ShowInfo("set FIFO Size\n");
	for(i=0;i<AUTH_FIFO_SIZE;i++)
		auth_fifo[i].delflag=1;
	ShowInfo("set FIFO Size complete\n");

	ShowInfo("set max servers\n");
	for(i=0;i<MAX_SERVERS;i++)
		server_fd[i]=-1;
	ShowInfo("set max servers complete\n");
	//server port open & binding

	// Online user database init
      online_db = numdb_init();
	add_timer_func_list(waiting_disconnect_timer, "waiting_disconnect_timer");

	//login_fd=make_listen_port(login_port);
	if (bind_ip_set_)
		login_fd = make_listen_bind(inet_addr(bind_ip_str),login_port);
	else
		login_fd = make_listen_bind(INADDR_ANY,login_port);

	//Auth start
	ShowInfo("Running mmo_auth_sqldb_init()\n");
	mmo_auth_sqldb_init();
	ShowInfo("finished mmo_auth_sqldb_init()\n");

	//Read account information.
	read_gm_account();

	//set default parser as parse_login function
	set_defaultparse(parse_login);

	// ban deleter timer - 1 minute term
	ShowStatus("add interval tic (ip_ban_check)....\n");
	add_timer_func_list(ip_ban_check,"ip_ban_check");
	i=add_timer_interval(gettick()+10, ip_ban_check,0,0,60*1000);

	if (console) {
		set_defaultconsoleparse(parse_console);
		start_console();
	}

	ShowStatus("The login-server is "CL_GREEN"ready"CL_RESET" (Server is listening on the port %d).\n\n", login_port);

	return 0;
}
