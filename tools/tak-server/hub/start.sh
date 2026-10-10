#!/bin/sh
# Starts the TAK federation hub in one container: the policy service, the plugin manager, the
# broker, and the SWAK test plugins when tools/tak_server.py enables them.
set -eu
cd /opt/tak/federation-hub
export JDK_JAVA_OPTIONS="-Dio.netty.tmpdir=/opt/tak/federation-hub/tmp \
 -Djava.io.tmpdir=/opt/tak/federation-hub/tmp -Dio.netty.native.workdir=/opt/tak/federation-hub/tmp \
 -DIGNITE_UPDATE_NOTIFIER=false -Djava.net.preferIPv4Stack=true"
mkdir -p tmp logs policies
java -Xmx512m -jar jars/federation-hub-policy.jar > logs/policy.log 2>&1 &
sleep 20
java -Xmx256m -jar jars/federation-hub-plugin-manager.jar > logs/plugin-manager.log 2>&1 &
java -Xmx768m --add-opens java.base/jdk.internal.misc=ALL-UNNAMED \
 -Dio.netty.tryReflectionSetAccessible=true -jar jars/federation-hub-broker.jar > logs/broker.log 2>&1 &
if [ -f plugins/SwakHubPlugin.java ]; then
    sleep 20
    # The plugins use the SDK and the Ignite libraries of the plugin manager.
    CLASSPATH="jars/federation-hub-plugins-sdk.jar:plugin-lib/*"
    # Ignite needs these packages open on Java 17. A boot jar gets them from its manifest.
    OPENS=""
    for package in java.base/jdk.internal.misc java.base/sun.nio.ch java.base/java.io \
        java.base/java.nio java.base/java.util java.base/java.util.concurrent \
        java.base/java.util.concurrent.locks java.base/java.util.concurrent.atomic \
        java.base/java.lang java.base/java.lang.invoke java.base/java.math java.sql/java.sql \
        java.base/sun.reflect.generics.reflectiveObjects java.management/com.sun.jmx.mbeanserver \
        jdk.internal.jvmstat/sun.jvmstat.monitor jdk.management/com.sun.management.internal; do
        OPENS="$OPENS --add-opens=$package=ALL-UNNAMED"
    done
    java -Xmx256m $OPENS -cp "$CLASSPATH" plugins/SwakHubPlugin.java interceptor > logs/plugin-interceptor.log 2>&1 &
    java -Xmx256m $OPENS -cp "$CLASSPATH" plugins/SwakHubPlugin.java sender SwakTest > logs/plugin-sender.log 2>&1 &
fi
wait
