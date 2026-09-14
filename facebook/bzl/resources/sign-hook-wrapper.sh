#!/bin/sh
set -xe

# Arguments:
#   $1 - path to shared build directory (KERNEL_TREE)
#   $2 - signing key name
#   $3 - path to the sign-sandcastle-spec.json template
KERNEL_TREE="$1"
SIGN_KEY="$2"
SPEC_TEMPLATE="$3"

# Buck's genrule context strips PATH. Ensure common tool locations are present.
export PATH="/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin:$PATH"


# Start the signature waiter in a background subshell
(
    while [ ! -e "$KERNEL_TREE/do-signature" -a \
            ! -e "$KERNEL_TREE/abort-signature" -a \
            -e "$KERNEL_TREE" ]; do
        sleep 10
    done
    # If the build dir doesn't exist any more then assume we're cleaning up.
    # If the abort touch file is set then just exit the subshell.
    if [ -e "$KERNEL_TREE/abort-signature" -o \
         ! -e "$KERNEL_TREE" ]; then
        exit
    fi

    # /failed-signature is a signal file to the build process to signify that
    # the signature failed.  If it exists then the build process will fail.
    # Sign modules via SandcastleKernelBuildCommand (Autograph-whitelisted)
    SIGN_TMPDIR=$(mktemp -d)
    SIGN_LOG=$SIGN_TMPDIR/signing.log
    (
        set -xeo pipefail
        tar -cf $SIGN_TMPDIR/unsigned_modules.tar -C "$KERNEL_TREE" .
        EVERSTORE_HANDLE=$(clowder put --fbtype EVERSTORE_LINUX_KERNEL $SIGN_TMPDIR/unsigned_modules.tar)
        KERNEL_COMMIT=$(git rev-parse HEAD)

        # Build the scutil job spec from the JSON template (no envsubst on aarch64)
        SPEC=$(sed -e "s|\$EVERSTORE_HANDLE|$EVERSTORE_HANDLE|g" \
                   -e "s|\$SIGN_KEY|$SIGN_KEY|g" \
                   -e "s|\$SANDCASTLE_NONCE|$SANDCASTLE_NONCE|g" \
                   -e "s|\$KERNEL_COMMIT|$KERNEL_COMMIT|g" \
                   "$SPEC_TEMPLATE")

        SCUTIL_OUTPUT=$(scutil create "$SPEC" --await --follow-retries -v json)
        # This will be the first instance ID. If it was retried, `scutil create` will
        # follow the retries but return the **original** instance ID.
        INSTANCE_ID=$(echo "$SCUTIL_OUTPUT" | jq -r '.id')
        # So get the final instance id from await. Because await will return the latest instance
        INSTANCE_ID=$(scutil await --follow-retries -v json $INSTANCE_ID | jq -r '.[0].id')

        SIGNED_HANDLE=$(scutil get-log $INSTANCE_ID | awk '/==OUTPUT MARKER 0==/{print $NF}')
        clowder get $SIGNED_HANDLE $SIGN_TMPDIR/signed_modules.tar
        tar -xf $SIGN_TMPDIR/signed_modules.tar -C "$KERNEL_TREE"
    ) > "$SIGN_LOG" 2>&1 || touch "$KERNEL_TREE/failed-signature"

    echo "=== Signing log ===" >&2
    cat "$SIGN_LOG" >&2
    echo "=== End signing log ===" >&2
    rm -rf "$SIGN_TMPDIR"

    rm "$KERNEL_TREE/do-signature"
) &
