#!/bin/sh
# Starts the TAK federation hub policy service, then the broker, in one container.
set -eu
cd /opt/tak/federation-hub
export JDK_JAVA_OPTIONS="-Dio.netty.tmpdir=/opt/tak/federation-hub/tmp \
 -Djava.io.tmpdir=/opt/tak/federation-hub/tmp -Dio.netty.native.workdir=/opt/tak/federation-hub/tmp \
 -DIGNITE_UPDATE_NOTIFIER=false -Djava.net.preferIPv4Stack=true"
mkdir -p tmp logs policies
java -Xmx512m -jar jars/federation-hub-policy.jar > logs/policy.log 2>&1 &
sleep 20
java -Xmx768m --add-opens java.base/jdk.internal.misc=ALL-UNNAMED \
 -Dio.netty.tryReflectionSetAccessible=true -jar jars/federation-hub-broker.jar > logs/broker.log 2>&1 &
wait
