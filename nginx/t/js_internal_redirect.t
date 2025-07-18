#!/usr/bin/perl

# (C) Dmitry Volyntsev
# (C) Nginx, Inc.

# Tests for http njs module, internalRedirect method.

###############################################################################

use warnings;
use strict;

use Test::More;
use Socket qw/ CRLF /;

BEGIN { use FindBin; chdir($FindBin::Bin); }

use lib 'lib';
use Test::Nginx;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http rewrite/)
	->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    js_import test.js;

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location /njs {
            js_content test.njs;
        }

        location /test {
            js_content test.redirect;
        }

        location /redirect {
            internal;
            return 200 redirect$arg_b;
        }

        location /destroyed_ctx {
            js_content test.destroyed_ctx;
        }

        location @named {
            return 200 named;
        }
    }
}

EOF

$t->write_file('test.js', <<EOF);
    function test_njs(r) {
        r.return(200, njs.version);
    }

    function redirect(r) {
        if (r.variables.arg_dest == 'named') {
            r.internalRedirect('\@named');

        } else if (r.variables.arg_unsafe) {
            r.internalRedirect('/red/../rect');

        } else if (r.variables.arg_quoted) {
            r.internalRedirect('/red%69rect');

        } else {
            if (r.variables.arg_a) {
                r.internalRedirect('/redirect?b=' + r.variables.arg_a);

            } else {
                r.internalRedirect('/redirect');
            }
        }
    }

    function destroyed_ctx(r) {
        try {
            r.return(200);

        } catch (e) {
            r.internalRedirect("\@sub");
        }
    }

    export default {njs:test_njs, redirect, destroyed_ctx};

EOF

$t->try_run('no njs available')->plan(5);

###############################################################################

like(http_get('/test'), qr/redirect/s, 'redirect');
like(http_get('/test?a=A'), qr/redirectA/s, 'redirect with args');
like(http_get('/test?dest=named'), qr/named/s, 'redirect to named location');

like(http_get('/test?unsafe=1'), qr/500 Internal Server/s,
	'unsafe redirect');
like(http_get('/test?quoted=1'), qr/200 .*redirect/s,
	'quoted redirect');
get('/destroyed_ctx', 'If-Match: tt');

###############################################################################

sub get {
    my ($url, @headers) = @_;
    return http(
        "GET $url HTTP/1.1" . CRLF .
        'Host: localhost' . CRLF .
        'Connection: close' . CRLF .
        join(CRLF, @headers) . CRLF . CRLF
    );
}

################################################################################
