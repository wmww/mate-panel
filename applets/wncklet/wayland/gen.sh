PROTO_NAME=wlr-foreign-toplevel-management-unstable-v1
wayland-scanner -c client-header ${PROTO_NAME}.xml ${PROTO_NAME}-client.h
wayland-scanner -c private-code ${PROTO_NAME}.xml ${PROTO_NAME}.c
