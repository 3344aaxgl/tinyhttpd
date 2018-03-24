/* match.c - simple shell-style filename matcher
**
** Only does ? * and **, and multiple patterns separated by |.  Returns 1 or 0.
**
** Copyright � 1995,2000 by Jef Poskanzer <jef@mail.acme.com>.
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
** OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
** HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
** SUCH DAMAGE.
*/


#include <string.h>

#include "match.h"

static int match_one( const char* pattern, int patternlen, const char* string );

int
match( const char* pattern, const char* string )
    {
    const char* or;

    for (;;)
	{
	or = strchr( pattern, '|' );/*是否以|做分隔符*/
	if ( or == (char*) 0 )
	    return match_one( pattern, strlen( pattern ), string );
	if ( match_one( pattern, or - pattern, string ) )
	    return 1;
	pattern = or + 1;
	}
    }


static int
match_one( const char* pattern, int patternlen, const char* string )
    {
    const char* p;
    /*以当前指向的位置减去起始小于整个字符串的长度来判断是否遍历完
	 比使用0来判断适用性好*/
    for ( p = pattern; p - pattern < patternlen; ++p, ++string )
	{
	if ( *p == '?' && *string != '\0' )
	    continue;
	if ( *p == '*' )
	    {
	    int i, pl;
	    ++p;
	    if ( *p == '*' )/* 两个**匹配任何字符串 */
		{
		/* Double-wildcard matches anything. */
		++p;
		i = strlen( string );
		}
	    else
		/* Single-wildcard matches anything but slash. */
		i = strcspn( string, "/" );
	    pl = patternlen - ( p - pattern );/* 除去* */
	    for ( ; i >= 0; --i )
		if ( match_one( p, pl, &(string[i]) ) )/*递归进行判断*/
		    return 1;
	    return 0;
	    }
	if ( *p != *string )
	    return 0;
	}
    if ( *string == '\0' )/*完全匹配*/
	return 1;
    return 0;
    }
