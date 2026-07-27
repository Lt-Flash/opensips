Charts for the rest_client response cache PR.

Measured on a single Debian 13 host over loopback, so the HTTP round trip
here is as cheap as it can possibly be; against a real origin the gap is
wider, not narrower.
