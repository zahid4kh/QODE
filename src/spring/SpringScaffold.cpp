#include "spring/SpringScaffold.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace SpringScaffold {

QString title(Kind kind)
{
    switch (kind) {
    case Kind::Controller: return QStringLiteral("Controller");
    case Kind::Service: return QStringLiteral("Service");
    case Kind::Entity: return QStringLiteral("Entity");
    case Kind::Repository: return QStringLiteral("Repository");
    case Kind::Configuration: return QStringLiteral("Configuration");
    case Kind::ExceptionHandler: return QStringLiteral("Exception Handler");
    }
    return {};
}

QString suffix(Kind kind)
{
    switch (kind) {
    case Kind::Controller: return QStringLiteral("Controller");
    case Kind::Service: return QStringLiteral("Service");
    case Kind::Repository: return QStringLiteral("Repository");
    case Kind::Configuration: return QStringLiteral("Configuration");
    case Kind::ExceptionHandler: return QStringLiteral("ExceptionHandler");
    case Kind::Entity: return {};
    }
    return {};
}

QString pathFor(const QString &name)
{
    // UserProfile -> user-profile -> user-profiles
    QString kebab;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name[i];
        if (c.isUpper() && i > 0)
            kebab += QLatin1Char('-');
        kebab += c.toLower();
    }
    if (kebab.endsWith(QLatin1Char('s')) || kebab.endsWith(QLatin1Char('x')))
        kebab += QStringLiteral("es");
    else if (kebab.size() > 1 && kebab.endsWith(QLatin1Char('y')) && !QStringLiteral("aeiou").contains(kebab[kebab.size() - 2]))
        kebab = kebab.left(kebab.size() - 1) + QStringLiteral("ies");
    else
        kebab += QLatin1Char('s');
    return QLatin1Char('/') + kebab;
}

QString packageOf(const QString &dir, const QStringList &sourceRoots, bool *kotlin)
{
    const QString d = QDir::cleanPath(dir);
    for (const QString &rootRaw : sourceRoots) {
        const QString root = QDir::cleanPath(rootRaw);
        if (d == root || d.startsWith(root + QLatin1Char('/'))) {
            if (kotlin)
                *kotlin = root.endsWith(QLatin1String("/kotlin"));
            return d.mid(root.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts).join(QLatin1Char('.'));
        }
    }
    return {};
}

namespace {

QString lines(const QStringList &l)
{
    return l.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString javaType(const QString &t)
{
    return t == QLatin1String("Int") ? QStringLiteral("Integer") : t;
}

QString importFor(const QString &t)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("BigDecimal"), QStringLiteral("java.math.BigDecimal")}, {QStringLiteral("LocalDate"), QStringLiteral("java.time.LocalDate")},
        {QStringLiteral("LocalDateTime"), QStringLiteral("java.time.LocalDateTime")}, {QStringLiteral("Instant"), QStringLiteral("java.time.Instant")},
        {QStringLiteral("UUID"), QStringLiteral("java.util.UUID")}};
    return map.value(t);
}

// "String" -> `String = ""`; types without an obvious default are nullable.
QString kotlinDecl(const QString &t)
{
    static const QHash<QString, QString> defaults = {
        {QStringLiteral("String"), QStringLiteral("\"\"")}, {QStringLiteral("Int"), QStringLiteral("0")}, {QStringLiteral("Long"), QStringLiteral("0L")},
        {QStringLiteral("Double"), QStringLiteral("0.0")}, {QStringLiteral("Float"), QStringLiteral("0f")}, {QStringLiteral("Boolean"), QStringLiteral("false")}};
    const auto it = defaults.constFind(t);
    return it != defaults.constEnd() ? t + QStringLiteral(" = ") + *it : t + QStringLiteral("? = null");
}

} // namespace

QVector<Field> parseFields(const QString &spec, QString *error)
{
    QVector<Field> out;
    static const QRegularExpression ident(QStringLiteral(R"(^[A-Za-z_][A-Za-z0-9_]*$)"));
    for (const QString &part : spec.split(QRegularExpression(QStringLiteral(R"([,;\s]+)")), Qt::SkipEmptyParts)) {
        const int colon = part.indexOf(QLatin1Char(':'));
        QString name = colon < 0 ? part : part.left(colon);
        QString t = colon < 0 ? QStringLiteral("String") : part.mid(colon + 1);
        if (t == QLatin1String("Integer") || t == QLatin1String("int"))
            t = QStringLiteral("Int");
        else if (t == QLatin1String("bool") || t == QLatin1String("boolean"))
            t = QStringLiteral("Boolean");
        else if (!t.isEmpty() && t[0].isLower())
            t[0] = t[0].toUpper();
        if (!ident.match(name).hasMatch() || !ident.match(t).hasMatch() || name == QLatin1String("id")) {
            if (error)
                *error = QObject::tr("“%1” is not a valid field (use name:Type; the id is added for you).").arg(part);
            return {};
        }
        name[0] = name[0].toLower();
        for (const Field &f : out)
            if (f.name == name) {
                if (error)
                    *error = QObject::tr("Field “%1” is listed twice.").arg(name);
                return {};
            }
        out.append({name, t});
    }
    return out;
}

Result generate(Kind kind, const QString &rawName, const QString &dir, const QStringList &sourceRoots, const QVector<Field> &fields)
{
    Result r;
    bool kotlin = true;
    const QString pkg = packageOf(dir, sourceRoots, &kotlin);
    if (pkg.isEmpty()) {
        r.error = QObject::tr("Pick a package folder below src/main/kotlin or src/main/java.");
        return r;
    }
    QString base = rawName.trimmed();
    static const QRegularExpression ident(QStringLiteral(R"(^[A-Za-z_][A-Za-z0-9_]*$)"));
    if (!ident.match(base).hasMatch()) {
        r.error = QObject::tr("“%1” is not a valid class name.").arg(base);
        return r;
    }
    base[0] = base[0].toUpper();
    const QString suf = suffix(kind);
    if (!suf.isEmpty() && base.endsWith(suf) && base.size() > suf.size())
        base.chop(suf.size());
    const QString cls = base + suf;
    const QString ext = kotlin ? QStringLiteral(".kt") : QStringLiteral(".java");
    r.path = QDir(dir).filePath(cls + ext);

    const QString semi = kotlin ? QString() : QStringLiteral(";");
    auto imp = [&](const QString &i) { return QStringLiteral("import ") + i + semi; };
    QStringList out;
    out << QStringLiteral("package %1%2").arg(pkg, semi) << QString();

    switch (kind) {
    case Kind::Controller:
        out << imp(QStringLiteral("org.springframework.web.bind.annotation.GetMapping"))
            << imp(QStringLiteral("org.springframework.web.bind.annotation.PathVariable"))
            << imp(QStringLiteral("org.springframework.web.bind.annotation.RequestMapping"))
            << imp(QStringLiteral("org.springframework.web.bind.annotation.RestController"));
        if (!kotlin)
            out << imp(QStringLiteral("java.util.List"));
        out << QString() << QStringLiteral("@RestController") << QStringLiteral("@RequestMapping(\"%1\")").arg(pathFor(base));
        if (kotlin)
            out << QStringLiteral("class %1 {").arg(cls) << QString()
                << QStringLiteral("    @GetMapping") << QStringLiteral("    fun list(): List<String> = emptyList()") << QString()
                << QStringLiteral("    @GetMapping(\"/{id}\")") << QStringLiteral("    fun get(@PathVariable id: Long): String = \"%1 $id\"").arg(base) << QStringLiteral("}");
        else
            out << QStringLiteral("public class %1 {").arg(cls) << QString()
                << QStringLiteral("    @GetMapping") << QStringLiteral("    public List<String> list() {") << QStringLiteral("        return List.of();") << QStringLiteral("    }") << QString()
                << QStringLiteral("    @GetMapping(\"/{id}\")") << QStringLiteral("    public String get(@PathVariable Long id) {") << QStringLiteral("        return \"%1 \" + id;").arg(base) << QStringLiteral("    }") << QStringLiteral("}");
        break;
    case Kind::Service:
        out << imp(QStringLiteral("org.springframework.stereotype.Service")) << QString() << QStringLiteral("@Service");
        out << (kotlin ? QStringLiteral("class %1 {\n}").arg(cls) : QStringLiteral("public class %1 {\n}").arg(cls));
        break;
    case Kind::Entity: {
        out << imp(QStringLiteral("jakarta.persistence.Entity")) << imp(QStringLiteral("jakarta.persistence.GeneratedValue"))
            << imp(QStringLiteral("jakarta.persistence.GenerationType")) << imp(QStringLiteral("jakarta.persistence.Id"));
        QStringList extra;
        for (const Field &f : fields) {
            const QString i = importFor(f.type);
            if (!i.isEmpty() && !extra.contains(i))
                extra << i;
        }
        extra.sort();
        for (const QString &i : extra)
            out << imp(i);
        out << QString() << QStringLiteral("@Entity");
        if (kotlin) {
            out << QStringLiteral("class %1(").arg(cls) << QStringLiteral("    @Id") << QStringLiteral("    @GeneratedValue(strategy = GenerationType.IDENTITY)")
                << QStringLiteral("    var id: Long? = null,");
            for (const Field &f : fields)
                out << QStringLiteral("    var %1: %2,").arg(f.name, kotlinDecl(f.type));
            out << QStringLiteral(")");
        } else {
            out << QStringLiteral("public class %1 {").arg(cls) << QString() << QStringLiteral("    @Id") << QStringLiteral("    @GeneratedValue(strategy = GenerationType.IDENTITY)")
                << QStringLiteral("    private Long id;");
            for (const Field &f : fields)
                out << QStringLiteral("    private %1 %2;").arg(javaType(f.type), f.name);
            out << QString();
            auto accessors = [&](const QString &type, const QString &name) {
                const QString cap = name.left(1).toUpper() + name.mid(1);
                out << QStringLiteral("    public %1 get%2() {").arg(type, cap) << QStringLiteral("        return %1;").arg(name) << QStringLiteral("    }") << QString()
                    << QStringLiteral("    public void set%1(%2 %3) {").arg(cap, type, name) << QStringLiteral("        this.%1 = %1;").arg(name) << QStringLiteral("    }") << QString();
            };
            accessors(QStringLiteral("Long"), QStringLiteral("id"));
            for (const Field &f : fields)
                accessors(javaType(f.type), f.name);
            out.removeLast();
            out << QStringLiteral("}");
        }
        break;
    }
    case Kind::Repository:
        out << imp(QStringLiteral("org.springframework.data.jpa.repository.JpaRepository")) << QString();
        out << (kotlin ? QStringLiteral("interface %1 : JpaRepository<%2, Long>").arg(cls, base)
                       : QStringLiteral("public interface %1 extends JpaRepository<%2, Long> {\n}").arg(cls, base));
        break;
    case Kind::Configuration:
        out << imp(QStringLiteral("org.springframework.context.annotation.Configuration")) << QString() << QStringLiteral("@Configuration");
        out << (kotlin ? QStringLiteral("class %1 {\n}").arg(cls) : QStringLiteral("public class %1 {\n}").arg(cls));
        break;
    case Kind::ExceptionHandler:
        out << imp(QStringLiteral("org.springframework.http.HttpStatus")) << imp(QStringLiteral("org.springframework.http.ProblemDetail"))
            << imp(QStringLiteral("org.springframework.web.bind.annotation.ExceptionHandler")) << imp(QStringLiteral("org.springframework.web.bind.annotation.RestControllerAdvice"))
            << QString() << QStringLiteral("@RestControllerAdvice");
        if (kotlin)
            out << QStringLiteral("class %1 {").arg(cls) << QString() << QStringLiteral("    @ExceptionHandler(IllegalArgumentException::class)")
                << QStringLiteral("    fun badRequest(e: IllegalArgumentException): ProblemDetail =")
                << QStringLiteral("        ProblemDetail.forStatusAndDetail(HttpStatus.BAD_REQUEST, e.message ?: \"Bad request\")") << QStringLiteral("}");
        else
            out << QStringLiteral("public class %1 {").arg(cls) << QString() << QStringLiteral("    @ExceptionHandler(IllegalArgumentException.class)")
                << QStringLiteral("    public ProblemDetail badRequest(IllegalArgumentException e) {")
                << QStringLiteral("        return ProblemDetail.forStatusAndDetail(HttpStatus.BAD_REQUEST, e.getMessage());") << QStringLiteral("    }") << QStringLiteral("}");
        break;
    }
    r.content = lines(out);
    return r;
}


QVector<Result> generateCrud(const QString &rawName, const QVector<Field> &fields, const QString &dir, const QStringList &sourceRoots)
{
    const Result entity = generate(Kind::Entity, rawName, dir, sourceRoots, fields);
    if (!entity.ok())
        return {entity};
    bool kotlin = true;
    const QString pkg = packageOf(dir, sourceRoots, &kotlin);
    const QString base = QFileInfo(entity.path).completeBaseName();
    const QString var = base.left(1).toLower() + base.mid(1);
    const QString semi = kotlin ? QString() : QStringLiteral(";");
    const QString ext = kotlin ? QStringLiteral(".kt") : QStringLiteral(".java");
    auto imp = [&](const QString &i) { return QStringLiteral("import ") + i + semi; };
    auto cap = [](const QString &n) { return n.left(1).toUpper() + n.mid(1); };
    auto S = [](const char *t) { return QString::fromLatin1(t); };
    auto fmt = [&](const char *t) { return S(t).replace(QStringLiteral("%1"), base).replace(QStringLiteral("%2"), var); }; // %1 = entity, %2 = variable

    QVector<Result> all{entity, generate(Kind::Repository, base, dir, sourceRoots)};

    Result svc;
    svc.path = QDir(dir).filePath(base + QStringLiteral("Service") + ext);
    QStringList s;
    s << QStringLiteral("package %1%2").arg(pkg, semi) << QString()
      << imp(S("org.springframework.http.HttpStatus")) << imp(S("org.springframework.stereotype.Service")) << imp(S("org.springframework.web.server.ResponseStatusException"));
    if (!kotlin)
        s << imp(S("java.util.List"));
    s << QString() << S("@Service");
    if (kotlin) {
        s << fmt("class %1Service(private val repository: %1Repository) {") << QString()
          << fmt("    fun findAll(): List<%1> = repository.findAll()") << QString()
          << fmt("    fun findById(id: Long): %1 =")
          << fmt("        repository.findById(id).orElseThrow { ResponseStatusException(HttpStatus.NOT_FOUND, \"%1 $id not found\") }") << QString()
          << fmt("    fun create(%2: %1): %1 = repository.save(%2)") << QString()
          << fmt("    fun update(id: Long, %2: %1): %1 {") << S("        val existing = findById(id)");
        for (const Field &f : fields)
            s << QStringLiteral("        existing.%1 = %2.%1").arg(f.name, var);
        s << S("        return repository.save(existing)") << S("    }") << QString()
          << S("    fun delete(id: Long) {") << S("        findById(id)") << S("        repository.deleteById(id)") << S("    }") << S("}");
    } else {
        s << fmt("public class %1Service {") << QString()
          << fmt("    private final %1Repository repository;") << QString()
          << fmt("    public %1Service(%1Repository repository) {") << S("        this.repository = repository;") << S("    }") << QString()
          << fmt("    public List<%1> findAll() {") << S("        return repository.findAll();") << S("    }") << QString()
          << fmt("    public %1 findById(Long id) {")
          << fmt("        return repository.findById(id).orElseThrow(() -> new ResponseStatusException(HttpStatus.NOT_FOUND, \"%1 \" + id + \" not found\"));")
          << S("    }") << QString()
          << fmt("    public %1 create(%1 %2) {") << fmt("        return repository.save(%2);") << S("    }") << QString()
          << fmt("    public %1 update(Long id, %1 %2) {") << fmt("        %1 existing = findById(id);");
        for (const Field &f : fields)
            s << QStringLiteral("        existing.set%1(%2.get%1());").arg(cap(f.name), var);
        s << S("        return repository.save(existing);") << S("    }") << QString()
          << S("    public void delete(Long id) {") << S("        findById(id);") << S("        repository.deleteById(id);") << S("    }") << S("}");
    }
    svc.content = lines(s);
    all << svc;

    Result ctl;
    ctl.path = QDir(dir).filePath(base + QStringLiteral("Controller") + ext);
    QStringList c;
    c << QStringLiteral("package %1%2").arg(pkg, semi) << QString() << imp(S("org.springframework.http.HttpStatus"));
    for (const char *a : {"DeleteMapping", "GetMapping", "PathVariable", "PostMapping", "PutMapping", "RequestBody", "RequestMapping", "ResponseStatus", "RestController"})
        c << imp(QStringLiteral("org.springframework.web.bind.annotation.") + S(a));
    if (!kotlin)
        c << imp(S("java.util.List"));
    c << QString() << S("@RestController") << QStringLiteral("@RequestMapping(\"%1\")").arg(pathFor(base));
    if (kotlin) {
        c << fmt("class %1Controller(private val service: %1Service) {") << QString()
          << S("    @GetMapping") << fmt("    fun list(): List<%1> = service.findAll()") << QString()
          << S("    @GetMapping(\"/{id}\")") << fmt("    fun get(@PathVariable id: Long): %1 = service.findById(id)") << QString()
          << S("    @PostMapping") << S("    @ResponseStatus(HttpStatus.CREATED)") << fmt("    fun create(@RequestBody %2: %1): %1 = service.create(%2)") << QString()
          << S("    @PutMapping(\"/{id}\")") << fmt("    fun update(@PathVariable id: Long, @RequestBody %2: %1): %1 = service.update(id, %2)") << QString()
          << S("    @DeleteMapping(\"/{id}\")") << S("    @ResponseStatus(HttpStatus.NO_CONTENT)") << S("    fun delete(@PathVariable id: Long) = service.delete(id)") << S("}");
    } else {
        c << fmt("public class %1Controller {") << QString()
          << fmt("    private final %1Service service;") << QString()
          << fmt("    public %1Controller(%1Service service) {") << S("        this.service = service;") << S("    }") << QString()
          << S("    @GetMapping") << fmt("    public List<%1> list() {") << S("        return service.findAll();") << S("    }") << QString()
          << S("    @GetMapping(\"/{id}\")") << fmt("    public %1 get(@PathVariable Long id) {") << S("        return service.findById(id);") << S("    }") << QString()
          << S("    @PostMapping") << S("    @ResponseStatus(HttpStatus.CREATED)") << fmt("    public %1 create(@RequestBody %1 %2) {") << fmt("        return service.create(%2);") << S("    }") << QString()
          << S("    @PutMapping(\"/{id}\")") << fmt("    public %1 update(@PathVariable Long id, @RequestBody %1 %2) {") << fmt("        return service.update(id, %2);") << S("    }") << QString()
          << S("    @DeleteMapping(\"/{id}\")") << S("    @ResponseStatus(HttpStatus.NO_CONTENT)") << S("    public void delete(@PathVariable Long id) {") << S("        service.delete(id);") << S("    }") << S("}");
    }
    ctl.content = lines(c);
    all << ctl;
    return all;
}

} // namespace SpringScaffold
