#!/bin/sh
# Starts the TAK Server configuration, messaging, and API services in one container.
set -eu
cd /opt/tak
export IGNITE_HOME=/opt/tak/ignite
export JDK_JAVA_OPTIONS="-Dloader.path=WEB-INF/lib-provided,WEB-INF/lib,WEB-INF/classes,file:lib/ \
 -Djava.net.preferIPv4Stack=true -Djava.security.egd=file:/dev/./urandom -DIGNITE_UPDATE_NOTIFIER=false \
 -DIGNITE_QUIET=true -Dio.netty.tmpdir=/opt/tak/tmp -Djava.io.tmpdir=/opt/tak/tmp \
 -Dio.netty.native.workdir=/opt/tak/tmp -Djdk.tls.client.protocols=TLSv1.2 \
 --add-opens=java.base/sun.security.pkcs=ALL-UNNAMED --add-opens=java.base/sun.security.pkcs10=ALL-UNNAMED \
 --add-opens=java.base/sun.security.util=ALL-UNNAMED --add-opens=java.base/sun.security.x509=ALL-UNNAMED \
 --add-opens=java.base/sun.security.tools.keytool=ALL-UNNAMED --add-opens=java.base/jdk.internal.misc=ALL-UNNAMED \
 --add-opens=java.base/sun.nio.ch=ALL-UNNAMED --add-opens=java.management/com.sun.jmx.mbeanserver=ALL-UNNAMED \
 --add-opens=jdk.internal.jvmstat/sun.jvmstat.monitor=ALL-UNNAMED \
 --add-opens=java.base/sun.reflect.generics.reflectiveObjects=ALL-UNNAMED \
 --add-opens=jdk.management/com.sun.management.internal=ALL-UNNAMED --add-opens=java.base/java.io=ALL-UNNAMED \
 --add-opens=java.base/java.nio=ALL-UNNAMED --add-opens=java.base/java.util=ALL-UNNAMED \
 --add-opens=java.base/java.util.concurrent=ALL-UNNAMED --add-opens=java.base/java.util.concurrent.locks=ALL-UNNAMED \
 --add-opens=java.base/java.util.concurrent.atomic=ALL-UNNAMED --add-opens=java.base/java.lang=ALL-UNNAMED \
 --add-opens=java.base/java.lang.invoke=ALL-UNNAMED --add-opens=java.base/java.math=ALL-UNNAMED \
 --add-opens=java.sql/java.sql=ALL-UNNAMED --add-opens=java.base/javax.net.ssl=ALL-UNNAMED \
 --add-opens=java.base/java.net=ALL-UNNAMED --add-opens=jdk.unsupported/sun.misc=ALL-UNNAMED \
 --add-opens=java.base/java.lang.ref=ALL-UNNAMED --add-opens=java.base/java.lang.reflect=ALL-UNNAMED \
 --add-opens=java.base/java.security=ALL-UNNAMED --add-opens=java.base/java.security.ssl=ALL-UNNAMED \
 --add-opens=java.base/java.security.cert=ALL-UNNAMED --add-opens=java.base/sun.security.rsa=ALL-UNNAMED \
 --add-opens=java.base/sun.security.ssl=ALL-UNNAMED --add-opens=java.base/sun.security.x500=ALL-UNNAMED \
 --add-opens=java.base/sun.security.pkcs12=ALL-UNNAMED --add-opens=java.base/sun.security.provider=ALL-UNNAMED \
 --add-opens=java.base/javax.security.auth.x500=ALL-UNNAMED"
mkdir -p tmp logs
java -Xmx768m -Dspring.profiles.active=config -Dkeystore.pkcs12.legacy -jar takserver.war > logs/config.log 2>&1 &
sleep 20
java -Xmx1024m -Dspring.profiles.active=messaging -jar takserver.war > logs/messaging.log 2>&1 &
java -Xmx1024m -Dspring.profiles.active=api -Dkeystore.pkcs12.legacy -jar takserver.war > logs/api.log 2>&1 &
wait
