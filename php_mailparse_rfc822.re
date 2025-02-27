/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available at through the world-wide-web at                           |
   | http://www.php.net/license/3_01.txt.                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Wez Furlong <wez@thebrainroom.com>                           |
   +----------------------------------------------------------------------+
 */

#include "php.h"
#include "php_mailparse.h"
#include "php_mailparse_rfc822.h"
#include "ext/standard/php_string.h"
#include "ext/standard/php_smart_string.h"
/*!re2c
CHAR = [\000-\177];
ALPHA = [\101-\132]|[\141-\172];
DIGIT = [\060-\071];
CTL = [\000-\037]|[\177];
CR = [\015];
LF = [\012];
SPACE = [\040];
HTAB = [\011];
CRLF = CR LF;
LWSPCHAR = SPACE|HTAB;
LWSP = ( CRLF? LWSPCHAR)+;
specials = [()<>@,;:\\".\[\]];
delimiters = (specials|LWSP);
*/

/*!re2c
NUL = [\000];
any = [\001-\377];
space = (HTAB|SPACE|CR|LF);
atom = [@,;:.%!?=/\[\]];
allspecials = (atom|[()<>"]|space);
other = any\allspecials;
*/

#define YYFILL(n)	if (YYCURSOR == YYLIMIT) goto stop
#define YYCTYPE		unsigned char
#define YYCURSOR	p
#define YYLIMIT		q
#define YYMARKER	r

#define DEBUG_RFC822_SCANNER	0

#if DEBUG_RFC822_SCANNER
# define DBG_STATE(lbl)		printf(lbl " %d:%c %d:%c\n", *YYCURSOR, *YYCURSOR, *start, *start)
#else
# define DBG_STATE(lbl)
#endif

#define ADD_ATOM_TOKEN()	do { if (tokens) { tokens->token = *start; tokens->value = start; tokens->valuelen = 1; tokens++; } ++*ntokens; } while (0)
#define REPORT_ERR(msg)		do { if (report_errors) zend_error(E_WARNING, "input is not rfc822 compliant: %s", msg); } while(0)
#define STR_FREE(ptr) if (ptr) { efree(ptr); }
/* Tokenize a header. tokens may be NULL, in which case the number of tokens are
   counted, allowing the caller to allocate enough room */
static void tokenize(const char *header, php_rfc822_token_t *tokens, int *ntokens, int report_errors)
{
	register const char *p, *q, *start;
	const char *r = NULL;
	int in_bracket = 0;

/* NB: parser assumes that the header has two bytes of NUL terminator */

	YYCURSOR = header;
	YYLIMIT = YYCURSOR + strlen(YYCURSOR) + 1;

	*ntokens = 0;

state_ground:
	start = YYCURSOR;

#if DEBUG_RFC822_SCANNER
printf("ground: start=%p limit=%p cursor=%p: [%d] %s\n", start, YYLIMIT, YYCURSOR, *YYCURSOR, YYCURSOR);
#endif

/*!re2c
	NUL					{	goto stop; }
	space+				{ 	DBG_STATE("SPACE"); goto state_ground; }
	(")"|"\\")			{ 	REPORT_ERR("token not valid in ground state"); goto state_ground; }
	"("					{	DBG_STATE("START COMMENT");
							if (tokens) {
								tokens->token = '(';
								tokens->value = start;
								tokens->valuelen = 0;
							}
							goto state_comment;
						}
	["] (any\["]|"\\\"")* ["]	{ 	DBG_STATE("QUOTE STRING");
							if (tokens) {
								tokens->token = '"';
								tokens->value = start + 1;
								tokens->valuelen = YYCURSOR - start - 2;
								tokens++;
							}
							++*ntokens;

							goto state_ground;
						}
	"<" ">"				{	DBG_STATE("NULL <>");
							ADD_ATOM_TOKEN();
							if (tokens) {
								tokens->token = 0;
								tokens->value = "";
								tokens->valuelen = 0;
								tokens++;
							}
							++*ntokens;
							start++;
							ADD_ATOM_TOKEN();
							goto state_ground;
						}
	"<" 				{ 	DBG_STATE("LANGLE");
							if (in_bracket) {
								REPORT_ERR("already in < bracket");
								goto state_ground;
							}
							in_bracket = 1;
							ADD_ATOM_TOKEN();
							goto state_ground;
						}
	">"					{	DBG_STATE("RANGLE");
							if (!in_bracket) {
								REPORT_ERR("not in < bracket");
								goto state_ground;
							}
							in_bracket = 0;
							ADD_ATOM_TOKEN();
							goto state_ground;
						}
	atom				{ 	DBG_STATE("ATOM"); ADD_ATOM_TOKEN(); goto state_ground; }
	other+				{	DBG_STATE("ANY");
							if (tokens) {
								tokens->token = 0;
								tokens->valuelen = YYCURSOR - start;
								tokens->value = start;
								tokens++;
							}
							++*ntokens;
							goto state_ground;
						}
*/

state_comment:
	{
		int comment_depth = 1;
		while (1) {
			if (*YYCURSOR == 0) {
				/* unexpected end of header */
				REPORT_ERR("unexpected end of header");
				/* fake a quoted string for this last token */
				if (tokens)
					tokens->token = '"';
				++*ntokens;
				return;
			} else if (*YYCURSOR == '(') {
				comment_depth++;
			} else if (*YYCURSOR == ')' && --comment_depth == 0) {
				/* end of nested comment sequence */
				YYCURSOR++;
				if (tokens)
					tokens->valuelen++;
				break;
			} else if (*YYCURSOR == '\\' && YYCURSOR[1]) {
				YYCURSOR++;
				if (tokens)
					tokens->valuelen++;
			}
			YYCURSOR++;
		}
		if (tokens) {
			tokens->valuelen = YYCURSOR - tokens->value;
			tokens++;
		}
		++*ntokens;
		goto state_ground;
	}
stop:
#if DEBUG_RFC822_SCANNER
	printf("STOPing parser ntokens=%d YYCURSOR=%p YYLIMIT=%p start=%p cursor=[%d] %s start=%s\n", *ntokens,
		YYCURSOR, YYLIMIT, start, *YYCURSOR, YYCURSOR, start);
#else
	;
#endif
}

PHP_MAILPARSE_API php_rfc822_tokenized_t *php_mailparse_rfc822_tokenize(const char *header, int report_errors)
{
	php_rfc822_tokenized_t *toks = ecalloc(1, sizeof(php_rfc822_tokenized_t));
	int len = strlen(header);

	toks->buffer = emalloc(len + 2);
	strcpy(toks->buffer, header);
	toks->buffer[len] = 0;
	toks->buffer[len+1] = 0; /* mini hack: the parser sometimes relies in this */

	tokenize(toks->buffer, NULL, &toks->ntokens, report_errors);
	toks->tokens = toks->ntokens ? ecalloc(toks->ntokens, sizeof(php_rfc822_token_t)) : NULL;
	tokenize(toks->buffer, toks->tokens, &toks->ntokens, report_errors);
	return toks;
}

PHP_MAILPARSE_API void php_rfc822_tokenize_free(php_rfc822_tokenized_t *toks)
{
	if (toks->tokens)
		efree(toks->tokens);
	efree(toks->buffer);
	efree(toks);
}

PHP_MAILPARSE_API char *php_rfc822_recombine_tokens(php_rfc822_tokenized_t *toks, int first_token, int n_tokens, int flags)
{
	char *ret = NULL;
	int i, upper, last_was_atom = 0, this_is_atom = 0, tok_equiv;
	size_t len = 1; /* for the NUL terminator */

	upper = first_token + n_tokens;
	if (upper > toks->ntokens)
		upper = toks->ntokens;

	for (i = first_token; i < upper; i++, last_was_atom = this_is_atom) {

		tok_equiv = toks->tokens[i].token;
		if (tok_equiv == '(' && flags & PHP_RFC822_RECOMBINE_COMMENTS_TO_QUOTES)
			tok_equiv = '"';

		if (flags & PHP_RFC822_RECOMBINE_IGNORE_COMMENTS && tok_equiv == '(')
			continue;
		if (flags & PHP_RFC822_RECOMBINE_COMMENTS_ONLY && tok_equiv != '(' && !(toks->tokens[i].token == '(' && flags & PHP_RFC822_RECOMBINE_COMMENTS_TO_QUOTES))
			continue;

		this_is_atom = php_rfc822_token_is_atom(toks->tokens[i].token);
		if (this_is_atom && last_was_atom && flags & PHP_RFC822_RECOMBINE_SPACE_ATOMS)
			len++; /* allow room for a space */

		if (flags & PHP_RFC822_RECOMBINE_INCLUDE_QUOTES && tok_equiv == '"')
			len += 2;

		len += toks->tokens[i].valuelen;
	}

	last_was_atom = this_is_atom = 0;

	ret = emalloc(len);

	for (i = first_token, len = 0; i < upper; i++, last_was_atom = this_is_atom) {
		const char *tokvalue;
		int toklen;

		tok_equiv = toks->tokens[i].token;
		if (tok_equiv == '(' && flags & PHP_RFC822_RECOMBINE_COMMENTS_TO_QUOTES)
			tok_equiv = '"';

		if (flags & PHP_RFC822_RECOMBINE_IGNORE_COMMENTS && tok_equiv == '(')
			continue;
		if (flags & PHP_RFC822_RECOMBINE_COMMENTS_ONLY && tok_equiv != '(' && !(toks->tokens[i].token == '(' && flags & PHP_RFC822_RECOMBINE_COMMENTS_TO_QUOTES))
			continue;

		tokvalue = toks->tokens[i].value;
		toklen = toks->tokens[i].valuelen;

		this_is_atom = php_rfc822_token_is_atom(toks->tokens[i].token);
		if (this_is_atom && last_was_atom && flags & PHP_RFC822_RECOMBINE_SPACE_ATOMS) {
			ret[len] = ' ';
			len++;
		}
		if (flags & PHP_RFC822_RECOMBINE_INCLUDE_QUOTES && tok_equiv == '"')
			ret[len++] = '"';

		if (toks->tokens[i].token == '(' && flags & PHP_RFC822_RECOMBINE_COMMENTS_TO_QUOTES) {
			/* don't include ( and ) in the output string */
			tokvalue++;
			toklen -= 2;
		}

		memcpy(ret + len, tokvalue, toklen);
		len += toklen;

		if (flags & PHP_RFC822_RECOMBINE_INCLUDE_QUOTES && tok_equiv == '"')
			ret[len++] = '"';

	}
	ret[len] = 0;

	if (flags & PHP_RFC822_RECOMBINE_STRTOLOWER)
		php_strtolower(ret, len);

	return ret;
}

static void parse_address_tokens(php_rfc822_tokenized_t *toks,
	php_rfc822_addresses_t *addrs, int *naddrs)
{
	int start_tok = 0, iaddr = 0, i, in_group = 0, group_lbl_start = 0, group_lbl_end = 0;
	int a_start, a_count; /* position and count for address part of a name */
	smart_string group_addrs = { 0, };
	char *address_value = NULL;

address:	/* mailbox / group */

	if (start_tok >= toks->ntokens) {
		/* the end */
		*naddrs = iaddr;
		smart_string_free(&group_addrs);
		return;
	}

	/* look ahead to determine if we are dealing with a group */
	for (i = start_tok; i < toks->ntokens; i++)
		if (toks->tokens[i].token != 0 && toks->tokens[i].token != '"')
			break;

	if (i < toks->ntokens && toks->tokens[i].token == ':') {
		/* it's a group */
		in_group = 1;
		group_lbl_start = start_tok;
		group_lbl_end = i;

		/* we want the address for the group to include the leading ":" and the trailing ";" */
		start_tok = i;
	}

mailbox:	/* addr-spec / phrase route-addr */
	if (start_tok >= toks->ntokens) {
		/* the end */
		*naddrs = iaddr;
		smart_string_free(&group_addrs);
		return;
	}

	/* skip spurious commas */
	while (start_tok < toks->ntokens && (toks->tokens[start_tok].token == ','
			|| toks->tokens[start_tok].token == ';'))
		start_tok++;

	/* look ahead: if we find a '<' before we find an '@', we are dealing with
	   a route-addr, otherwise we have an addr-spec */
	for (i = start_tok; i < toks->ntokens && toks->tokens[i].token != ';'
		&& toks->tokens[i].token != ',' && toks->tokens[i].token != '<'; i++)
		;

	/* the stuff from start_tok to i - 1 is the display name part */
	if (addrs && !in_group && i - start_tok > 0) {
		int j, has_comments = 0, has_strings = 0;
		switch(toks->tokens[i].token) {
			case ';': case ',': case '<':
				addrs->addrs[iaddr].name = php_rfc822_recombine_tokens(toks, start_tok, i - start_tok,
						PHP_RFC822_RECOMBINE_SPACE_ATOMS);
				break;
			default:
				/* it's only the display name if there are quoted strings or comments in there */
				for (j = start_tok; j < i; j++) {
					if (toks->tokens[j].token == '(')
						has_comments = 1;
					if (toks->tokens[j].token == '"')
						has_strings = 1;
				}
				if (has_comments && !has_strings) {
					addrs->addrs[iaddr].name = php_rfc822_recombine_tokens(toks, start_tok,
						i - start_tok,
						PHP_RFC822_RECOMBINE_SPACE_ATOMS | PHP_RFC822_RECOMBINE_COMMENTS_ONLY
						| PHP_RFC822_RECOMBINE_COMMENTS_TO_QUOTES
						);
				} else if (has_strings) {
					addrs->addrs[iaddr].name = php_rfc822_recombine_tokens(toks, start_tok, i - start_tok,
						PHP_RFC822_RECOMBINE_SPACE_ATOMS);

				}

		}

	}

	if (i < toks->ntokens && toks->tokens[i].token == '<') {
		int j;
		/* RFC822: route-addr = "<" [route] addr-spec ">" */
		/* look for the closing '>' and recombine as the address part */

		for (j = i; j < toks->ntokens && toks->tokens[j].token != '>'; j++)
			;

		if (addrs) {
			a_start = i;
			a_count = j-i;
			/* if an address is enclosed in <>, leave them out of the the
			 * address value that we return */
			if (toks->tokens[a_start].token == '<') {
				a_start++;
				a_count--;
			}
			address_value = php_rfc822_recombine_tokens(toks, a_start, a_count,
								PHP_RFC822_RECOMBINE_SPACE_ATOMS|
								PHP_RFC822_RECOMBINE_IGNORE_COMMENTS|
								PHP_RFC822_RECOMBINE_INCLUDE_QUOTES);
		}

		start_tok = ++j;
	} else {
		/* RFC822: addr-spec = local-part "@" domain */
		if (addrs) {
			a_start = start_tok;
			a_count = i - start_tok;
			/* if an address is enclosed in <>, leave them out of the the
			 * address value that we return */
			if (toks->tokens[a_start].token == '<') {
				a_start++;
				a_count--;
			}

			address_value = php_rfc822_recombine_tokens(toks, a_start, a_count,
								PHP_RFC822_RECOMBINE_SPACE_ATOMS|
								PHP_RFC822_RECOMBINE_IGNORE_COMMENTS|
								PHP_RFC822_RECOMBINE_INCLUDE_QUOTES);
		}
		start_tok = i;
	}

	if (addrs && address_value) {

		/* if no display name has been given, use the address */
		if (addrs->addrs[iaddr].name == NULL) {
			addrs->addrs[iaddr].name = estrdup(address_value);
		}

		if (in_group) {
			if (group_addrs.len)
				smart_string_appendl(&group_addrs, ",", 1);
			smart_string_appends(&group_addrs, address_value);
			efree(address_value);
		} else {
			addrs->addrs[iaddr].address = address_value;
		}
		address_value = NULL;
	}

	if (!in_group) {
		iaddr++;
		goto address;
	}
	/* still dealing with a group. If we find a ";", that's the end of the group */
	if ((start_tok < toks->ntokens && toks->tokens[start_tok].token == ';') || start_tok == toks->ntokens) {
		/* end of group */

		if (addrs) {
			smart_string_appendl(&group_addrs, ";", 1);
			smart_string_0(&group_addrs);
			addrs->addrs[iaddr].address = estrdup(group_addrs.c);
			group_addrs.len = 0;

			STR_FREE(addrs->addrs[iaddr].name);
			addrs->addrs[iaddr].name = php_rfc822_recombine_tokens(toks, group_lbl_start,
					group_lbl_end - group_lbl_start,
					PHP_RFC822_RECOMBINE_SPACE_ATOMS);

			addrs->addrs[iaddr].is_group = 1;
		}

		iaddr++;
		in_group = 0;
		start_tok++;
		goto address;
	}
	/* look for more mailboxes in this group */
	goto mailbox;
}

PHP_MAILPARSE_API php_rfc822_addresses_t *php_rfc822_parse_address_tokens(php_rfc822_tokenized_t *toks)
{
	php_rfc822_addresses_t *addrs = ecalloc(1, sizeof(php_rfc822_addresses_t));

	parse_address_tokens(toks, NULL, &addrs->naddrs);
    if (addrs->naddrs) {
        addrs->addrs = ecalloc(addrs->naddrs, sizeof(php_rfc822_address_t));
        parse_address_tokens(toks, addrs, &addrs->naddrs);
    }

	return addrs;
}

PHP_MAILPARSE_API void php_rfc822_free_addresses(php_rfc822_addresses_t *addrs)
{
	int i;
	for (i = 0; i < addrs->naddrs; i++) {
		if (addrs->addrs[i].name)
		STR_FREE(addrs->addrs[i].name);
		STR_FREE(addrs->addrs[i].address);
	}
	if (addrs->addrs)
		efree(addrs->addrs);
	efree(addrs);
}
void php_rfc822_print_addresses(php_rfc822_addresses_t *addrs)
{
	int i;
	printf("printing addresses %p\n", addrs); fflush(stdout);
	for (i = 0; i < addrs->naddrs; i++) {
		printf("addr %d: name=%s address=%s\n", i, addrs->addrs[i].name, addrs->addrs[i].address);
	}
}


void php_rfc822_print_tokens(php_rfc822_tokenized_t *toks)
{
	int i;
	for (i = 0; i < toks->ntokens; i++) {
		printf("token %d:  token=%d/%c len=%d value=%s\n", i, toks->tokens[i].token, toks->tokens[i].token,
			toks->tokens[i].valuelen, toks->tokens[i].value);
	}
}

PHP_FUNCTION(mailparse_test)
{
	char *header;
	size_t header_len;
	php_rfc822_tokenized_t *toks;
	php_rfc822_addresses_t *addrs;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &header, &header_len) == FAILURE) {
		RETURN_FALSE;
	}


#if 0
    {
	struct rfc822t *t = mailparse_rfc822t_alloc(header, NULL);
	for (i = 0; i < t->ntokens; i++) {
		printf("token %d:  token=%d/%c len=%d value=%s\n", i, t->tokens[i].token, t->tokens[i].token,
			t->tokens[i].len, t->tokens[i].ptr);

	}
	mailparse_rfc822t_free(t);

	printf("--- and now:\n");
    }
#endif

	toks = php_mailparse_rfc822_tokenize((const char*)header, 1);
	php_rfc822_print_tokens(toks);

	addrs = php_rfc822_parse_address_tokens(toks);
	php_rfc822_print_addresses(addrs);
	php_rfc822_free_addresses(addrs);

	php_rfc822_tokenize_free(toks);
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker syn=c
 * vim<600: sw=4 ts=4
 */
